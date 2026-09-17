/*
 * MXC6655 dual-accelerometer driver for CHUWI MiniBook.
 * Reads two MXC6655 accelerometers via raw I2C, computes hinge angle
 * for tablet mode detection, and provides orientation through a
 * settle-gated classifier. Tablet mode transitions are emitted via
 * uinput (SW_TABLET_MODE) and ACPI.
 */

#include "drivers.h"

#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <sys/ioctl.h>

#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <linux/uinput.h>
#include <linux/input.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

/* MXC6655 I2C registers */
#define MXC6655_ADDR		0x15
#define MXC6655_REG_XOUT	0x03	/* 6 bytes: XH,XL,YH,YL,ZH,ZL */
#define MXC6655_REG_DEVID	0x0E

#define ACPI_CALL_PATH		"/proc/acpi/call"
#define ACPI_LTSM_CMD		"\\_SB.ACMK.LTSM"
#define ACPI_MATCH_ID		"MDA6655"
#define I2C_UNBIND_DRIVER	"mxc4005"
#define MAX_I2C_BUS		20
#define MAX_INPUT_DEV		32
#define LID_SWITCH_NAME		"Lid Switch"
#define UINPUT_DEV_NAME		"MXC6655 Tablet Mode Control"

/* DRM_MODE_PANEL_ORIENTATION_* values (uapi/drm/drm_mode.h) */
#define PANEL_ORIENT_UNKNOWN	-1
#define PANEL_ORIENT_NORMAL	0
#define PANEL_ORIENT_BOTTOM_UP	1
#define PANEL_ORIENT_LEFT_UP	2
#define PANEL_ORIENT_RIGHT_UP	3
#define DRM_PANEL_ORIENT_PROP	"panel orientation"
#define MAX_DRM_CARD		4

/* GMTR PARB thresholds from DSDT \_SB.ACMK.GMTR */
#define GMTR_TABLET_THRESH	185.0f
#define GMTR_LAPTOP_THRESH	175.0f
#define GMTR_MIN_ANGLE		30.0f
#define GMTR_DEBOUNCE		5
#define GMTR_POLL_MS		50

/* Both sensors' Y axes point along the hinge; past this the X-Z plane the
 * hinge angle is computed from no longer carries enough of gravity. */
#define HINGE_AXIS_MAX		0.9f

/* Orientation settle-filter constants */
#define ORIENT_BUF_SIZE		20
#define ORIENT_VARIANCE_THRESH	0.01f
#define ORIENT_STABLE_MIN	10
#define ORIENT_JUMP_LIMIT	2.4f

typedef struct {
	float x, y, z;
} Vec3;

/* Median-of-3 spike suppressor for the hinge-angle Z inputs */
typedef struct {
	float	buf[3];
	gint	count;
} MedianFilter;

typedef enum {
	MXC_ORIENT_NORMAL   = 0,
	MXC_ORIENT_LEFT     = 1,
	MXC_ORIENT_INVERTED = 2,
	MXC_ORIENT_RIGHT    = 3,
} MxcOrientation;

typedef struct {
	/* Median filter for Z axis */
	float              z_median_buf[3];
	gint               z_median_count;

	/* Circular buffer for stability detection */
	float              buf_x[ORIENT_BUF_SIZE];
	float              buf_y[ORIENT_BUF_SIZE];
	float              buf_z[ORIENT_BUF_SIZE];
	gint               buf_idx;
	gint               buf_full;

	/* Consecutive quiet samples before classification is allowed */
	gint               quiet_count;

	/* Z-jump detection */
	float              ref_z;
	gboolean           have_ref;
	gboolean           primed;
} OrientState;

typedef struct {
	guint              timeout_id;
	gboolean           want_polling;
	gboolean           lid_closed;
	gint               lid_fd;
	guint              lid_watch_id;
	gint               i2c_fds[2];
	gint               uinput_fd;
	gint               ltsm_warned;

	/* Calibration matrices from DSDT GMTR */
	gint8              cal1[9];
	gint8              cal2[9];

	/* Previous Z values for hinge angle computation */
	float              prev_z1;
	float              prev_z2;

	/* Median filters for the hinge-angle Z inputs */
	MedianFilter       hinge_median[2];

	/* Tablet mode state machine */
	gint               mode;
	gint               t_count;
	gint               l_count;
	gint               n_count;

	/* Dynamic thresholds from DSDT GMTR */
	gint               tablet_thresh;
	float              laptop_thresh;
	float              min_angle;
	gint               debounce;
	gint               poll_ms;

	/* Orientation state */
	OrientState        orient;
	gint               cur_orient;
	gint               orient_debounce;
	gint               pending_orient;

	/* Degrees of static panel rotation to compensate for */
	gint               panel_deg;

	/* Raw accelerometer used for orientation (ACPI _CRS slot 1) */
	gint               rotation_idx;
	gchar             *rotation_controller;
} DrvData;

/* Rotation the compositor already applies for a given DRM panel orientation. */
static gint
panel_orient_degrees (gint drm_orient)
{
	switch (drm_orient) {
	case PANEL_ORIENT_LEFT_UP:
		return 90;
	case PANEL_ORIENT_BOTTOM_UP:
		return 180;
	case PANEL_ORIENT_RIGHT_UP:
		return 270;
	default:
		return 0;
	}
}

/* Subtract the statically-applied panel rotation from a sensor orientation. */
static gint
compose_orientation (gint mxc_orient, gint panel_deg)
{
	gint deg = ((mxc_orient * 90 - panel_deg) % 360 + 360) % 360;

	return deg / 90;
}

static gint
connector_panel_orientation (gint fd, drmModeConnectorPtr conn)
{
	for (gint i = 0; i < conn->count_props; i++) {
		drmModePropertyPtr prop = drmModeGetProperty (fd, conn->props[i]);
		gint value = PANEL_ORIENT_UNKNOWN;

		if (prop == NULL)
			continue;
		if (strcmp (prop->name, DRM_PANEL_ORIENT_PROP) == 0)
			value = (gint) conn->prop_values[i];
		drmModeFreeProperty (prop);
		if (value != PANEL_ORIENT_UNKNOWN)
			return value;
	}
	return PANEL_ORIENT_UNKNOWN;
}

static gint
card_panel_orientation (gint fd)
{
	drmModeResPtr res = drmModeGetResources (fd);
	gint orient = PANEL_ORIENT_UNKNOWN;

	if (res == NULL)
		return PANEL_ORIENT_UNKNOWN;

	for (gint i = 0; i < res->count_connectors; i++) {
		drmModeConnectorPtr conn;

		conn = drmModeGetConnectorCurrent (fd, res->connectors[i]);
		if (conn == NULL)
			continue;
		if (conn->connector_type == DRM_MODE_CONNECTOR_DSI)
			orient = connector_panel_orientation (fd, conn);
		drmModeFreeConnector (conn);
		if (orient != PANEL_ORIENT_UNKNOWN)
			break;
	}

	drmModeFreeResources (res);
	return orient;
}

static gint
read_panel_orientation (void)
{
	for (gint card = 0; card < MAX_DRM_CARD; card++) {
		g_autofree gchar *path = g_strdup_printf ("/dev/dri/card%d", card);
		gint fd = open (path, O_RDONLY | O_CLOEXEC);
		gint orient;

		if (fd < 0)
			continue;

		orient = card_panel_orientation (fd);
		close (fd);
		if (orient != PANEL_ORIENT_UNKNOWN)
			return orient;
	}
	return PANEL_ORIENT_UNKNOWN;
}

/*
 * Detect a statically-applied panel rotation (VBT patch, kernel cmdline
 * panel_orientation=, or an i915 quirk) so the driver does not stack its
 * own dynamic rotation on top of it.
 */
static gint
detect_panel_rotation (void)
{
	gint drm_orient = read_panel_orientation ();
	gint deg;

	if (drm_orient == PANEL_ORIENT_UNKNOWN) {
		g_message ("MXC6655: DRM panel orientation unknown, no compensation");
		return 0;
	}

	deg = panel_orient_degrees (drm_orient);
	g_message ("MXC6655: DRM panel orientation %d, compensating sensor "
		   "output by %d°, laptop mode reports orientation %d",
		   drm_orient, deg,
		   compose_orientation (MXC_ORIENT_RIGHT, deg));

	return deg;
}

static gint
i2c_xfer (gint fd, guint8 reg, guint8 *buf, gint len)
{
	struct i2c_msg msgs[2] = {
		{ .addr = MXC6655_ADDR, .flags = 0, .len = 1, .buf = &reg },
		{ .addr = MXC6655_ADDR, .flags = I2C_M_RD, .len = len, .buf = buf },
	};
	struct i2c_rdwr_ioctl_data data = { .msgs = msgs, .nmsgs = 2 };

	return ioctl (fd, I2C_RDWR, &data) < 0 ? -1 : 0;
}

/* Scale factor: 1/4096 converts 12-bit left-justified to g-units */
static gint
read_accel (gint fd, Vec3 *v)
{
	guint8 buf[6];

	if (i2c_xfer (fd, MXC6655_REG_XOUT, buf, 6) < 0)
		return -1;

	float scale = 0.00024414063f;
	v->x = (float)(gint)(gint16)((buf[0] << 8) | buf[1]) * scale;
	v->y = (float)(gint)(gint16)((buf[2] << 8) | buf[3]) * scale;
	v->z = (float)(gint)(gint16)((buf[4] << 8) | buf[5]) * scale;
	return 0;
}

static Vec3
calibrate (const Vec3 *v, const gint8 m[9])
{
	Vec3 r;

	r.x = m[0] * v->x + m[1] * v->y + m[2] * v->z;
	r.y = m[3] * v->x + m[4] * v->y + m[5] * v->z;
	r.z = m[6] * v->x + m[7] * v->y + m[8] * v->z;
	return r;
}

static gint
probe_bus (gint bus)
{
	g_autofree char *path = NULL;
	guint8 id;
	gint fd;

	path = g_strdup_printf ("/dev/i2c-%d", bus);
	fd = open (path, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return -1;
	if (i2c_xfer (fd, MXC6655_REG_DEVID, &id, 1) < 0) {
		close (fd);
		return -1;
	}
	return fd;
}

static gint
find_accels (gint fds[2], gchar *controllers[2])
{
	gint found = 0;
	GUdevClient *client;
	GList *list, *l;
	const gchar *subsystems[] = { "i2c", NULL };

	client = g_udev_client_new (subsystems);
	list = g_udev_client_query_by_subsystem (client, "i2c");

	for (l = list; l != NULL && found < 2; l = l->next) {
		GUdevDevice *dev = l->data;
		const gchar *name = g_udev_device_get_name (dev);
		const gchar *acpi_path;
		gint bus, fd;

		acpi_path = g_udev_device_get_sysfs_attr (dev, "firmware_node/path");
		if (acpi_path == NULL || strstr (acpi_path, ".I2C") == NULL)
			continue;

		bus = atoi (name + 4);
		fd = probe_bus (bus);
		if (fd >= 0) {
			g_debug ("MXC6655 found on %s (%s)", name, acpi_path);
			fds[found] = fd;
			controllers[found] = g_strdup (acpi_path);
			found++;
		}
	}

	g_list_free_full (list, g_object_unref);
	g_object_unref (client);
	return found;
}

static void
try_unbind (void)
{
	static const char *paths[] = {
		"/sys/bus/i2c/drivers/" I2C_UNBIND_DRIVER "/unbind",
		"/sys/bus/acpi/drivers/" I2C_UNBIND_DRIVER "/unbind",
	};

	for (gint i = 0; i < 2; i++) {
		gint fd = open (paths[i], O_WRONLY | O_CLOEXEC);
		if (fd < 0)
			continue;

		/* Try several possible indices in case they are enumerated differently */
		for (gint idx = 0; idx < 4; idx++) {
			g_autofree gchar *name = NULL;
			if (i == 0) /* I2C driver unbind */
				name = g_strdup_printf ("i2c-" ACPI_MATCH_ID ":%02d", idx);
			else /* ACPI driver unbind */
				name = g_strdup_printf (ACPI_MATCH_ID ":%02d", idx);

			if (write (fd, name, strlen (name)) > 0)
				g_debug ("Unbound %s via %s", name, paths[i]);
		}
		close (fd);
	}
	usleep (200000);
}

static gint
call_ltsm (gint mode, gboolean *warned)
{
	gint fd;
	g_autofree char *cmd = NULL;
	gint ret;

	fd = open (ACPI_CALL_PATH, O_WRONLY | O_CLOEXEC);
	if (fd < 0) {
		if (!*warned) {
			g_warning ("Cannot open %s: %s", ACPI_CALL_PATH, g_strerror (errno));
			*warned = TRUE;
		}
		return -1;
	}
	cmd = g_strdup_printf ("%s %d", ACPI_LTSM_CMD, mode);
	ret = write (fd, cmd, strlen (cmd));
	close (fd);
	return ret > 0 ? 0 : -1;
}

#define ACPI_GMTR_CMD		"\\_SB.ACMK.GMTR"
#define ACPI_CRS_CMD		"\\_SB.ACMK._CRS"

static ssize_t
acpi_call_read (const gchar *cmd, gchar *buf, gsize buf_size)
{
	gint fd;
	ssize_t len;

	fd = open (ACPI_CALL_PATH, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return -1;

	if (write (fd, cmd, strlen (cmd)) < 0) {
		close (fd);
		return -1;
	}

	len = read (fd, buf, buf_size - 1);
	close (fd);

	if (len <= 0)
		return -1;

	buf[len] = '\0';
	return len;
}

/* acpi_call renders Buffers as "{0x79, 0x35, ...}" and Packages as "[...]" */
static gchar **
split_acpi_list (const gchar *buf)
{
	gchar *end;
	g_autofree gchar *copy = NULL;
	gchar **elements;

	if (buf[0] != '{' && buf[0] != '[')
		return NULL;

	copy = g_strdup (buf + 1);
	end = strpbrk (copy, "}]");
	if (end)
		*end = '\0';

	elements = g_strsplit (copy, ",", -1);
	for (gint i = 0; elements[i] != NULL; i++)
		g_strstrip (elements[i]);
	return elements;
}

static gint
parse_acpi_package (const gchar *buf, guint64 *values, gint max_values)
{
	gchar **elements;
	gint count = 0;

	elements = split_acpi_list (buf);
	if (elements == NULL)
		return 0;

	for (gint i = 0; elements[i] != NULL && count < max_values; i++) {
		if (strlen (elements[i]) == 0)
			continue;
		values[count++] = g_ascii_strtoull (elements[i], NULL, 0);
	}
	g_strfreev (elements);
	return count;
}

static gint
parse_acpi_bytes (const gchar *buf, guint8 *bytes, gint max_bytes)
{
	gchar **elements;
	gint count = 0;

	elements = split_acpi_list (buf);
	if (elements == NULL)
		return 0;

	for (gint i = 0; elements[i] != NULL && count < max_bytes; i++) {
		guint64 value;

		if (strlen (elements[i]) == 0)
			continue;
		value = g_ascii_strtoull (elements[i], NULL, 0);
		if (value > 0xff)
			break;
		bytes[count++] = (guint8) value;
	}
	g_strfreev (elements);
	return count;
}

static void
apply_gmtr_values (DrvData *drv_data, const guint64 *values, gint count)
{
	for (gint i = 0; i < count && i < 9; i++)
		drv_data->cal1[i] = (gint8) values[i];

	for (gint i = 9; i < count && i < 18; i++)
		drv_data->cal2[i - 9] = (gint8) values[i];

	if (count > 18 && values[18] > 0)
		drv_data->tablet_thresh = (float) values[18];
	if (count > 19 && values[19] > 0)
		drv_data->laptop_thresh = (float) values[19];
	if (count > 20 && values[20] > 0)
		drv_data->min_angle = (float) values[20];
	if (count > 21 && values[21] > 0)
		drv_data->debounce = (gint) values[21];
	if (count > 22) {
		guint64 rate = values[22] & 0xff;

		drv_data->poll_ms = rate == 0 ? 100 : (gint) (1000 / rate);
		drv_data->poll_ms = CLAMP (drv_data->poll_ms, 10, 1000);
	}
	if (count > 23)
		g_debug ("GMTR axis mode %d, gate config %d",
			 (gint) (values[23] & 0xf),
			 (gint) ((values[23] >> 4) & 0xf));
}

static gboolean
load_gmtr (DrvData *drv_data)
{
	gchar buf[4096];
	guint64 values[32];
	gint count;

	drv_data->tablet_thresh = GMTR_TABLET_THRESH;
	drv_data->laptop_thresh = GMTR_LAPTOP_THRESH;
	drv_data->min_angle = GMTR_MIN_ANGLE;
	drv_data->debounce = GMTR_DEBOUNCE;
	drv_data->poll_ms = GMTR_POLL_MS;

	if (acpi_call_read (ACPI_GMTR_CMD, buf, sizeof (buf)) < 0)
		return FALSE;

	g_debug ("GMTR response: %s", buf);

	count = parse_acpi_package (buf, values, 32);
	if (count < 18)
		return FALSE;

	apply_gmtr_values (drv_data, values, count);
	g_debug ("Loaded %d values from GMTR calibration", count);
	return TRUE;
}

/*
 * Collect the ResourceSource strings (ACPI controller paths) from a _CRS
 * buffer, in resource order. They are the only printable backslash paths
 * a serial-bus resource list contains.
 */
static gint
extract_crs_sources (const guint8 *bytes, gint len, gchar *sources[], gint max)
{
	gint count = 0;
	gint i = 0;

	while (i < len && count < max) {
		gint start = i;

		while (i < len && bytes[i] >= 0x20 && bytes[i] < 0x7f)
			i++;

		if (i - start >= 8 && bytes[start] == '\\')
			sources[count++] = g_strndup ((const gchar *) &bytes[start],
						      i - start);

		if (i == start)
			i++;
	}
	return count;
}

static gint
match_controller (const gchar *controller, gchar *const controllers[2])
{
	for (gint i = 0; i < 2; i++) {
		if (controllers[i] != NULL && g_str_equal (controller, controllers[i]))
			return i;
	}
	return -1;
}

/*
 * Pick the orientation sensor the way the Windows driver does: the first
 * I2C resource listed in ACMK._CRS (its "slot 1"). On this firmware that
 * is the display accelerometer. Falls back to the first sensor found.
 */
static void
resolve_rotation_sensor (DrvData *drv_data, gchar *const controllers[2])
{
	gchar buf[4096];
	guint8 bytes[512];
	gchar *sources[4] = { NULL };
	gint len, count, idx;

	drv_data->rotation_idx = 0;
	g_debug ("Sensor controllers: [0]=%s [1]=%s",
		 controllers[0] != NULL ? controllers[0] : "none",
		 controllers[1] != NULL ? controllers[1] : "none");

	if (acpi_call_read (ACPI_CRS_CMD, buf, sizeof (buf)) < 0) {
		g_warning ("Cannot evaluate %s, using first sensor for orientation",
			   ACPI_CRS_CMD);
		return;
	}

	len = parse_acpi_bytes (buf, bytes, sizeof (bytes));
	count = extract_crs_sources (bytes, len, sources, 4);
	for (gint i = 0; i < count; i++)
		g_debug ("_CRS resource %d: %s", i, sources[i]);

	idx = count > 0 ? match_controller (sources[0], controllers) : -1;
	if (idx >= 0) {
		drv_data->rotation_idx = idx;
		g_free (drv_data->rotation_controller);
		drv_data->rotation_controller = g_strdup (sources[0]);
		g_message ("MXC6655: orientation sensor is %s (%s)",
			   idx == 0 ? "first" : "second", sources[0]);
	} else {
		g_warning ("Cannot match _CRS resources to sensors, "
			   "using first sensor for orientation");
	}

	for (gint i = 0; i < 4; i++)
		g_free (sources[i]);
}

static gint
setup_uinput (void)
{
	gint fd;
	struct uinput_setup setup;

	fd = open ("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return -1;

	if (ioctl (fd, UI_SET_EVBIT, EV_SW) < 0 ||
	    ioctl (fd, UI_SET_SWBIT, SW_TABLET_MODE) < 0)
		goto fail;

	memset (&setup, 0, sizeof (setup));
	setup.id.bustype = BUS_VIRTUAL;
	setup.id.vendor  = 0x4358;
	setup.id.product = 0x0001;
	g_snprintf (setup.name, UINPUT_MAX_NAME_SIZE, UINPUT_DEV_NAME);

	if (ioctl (fd, UI_DEV_SETUP, &setup) < 0 ||
	    ioctl (fd, UI_DEV_CREATE) < 0)
		goto fail;

	usleep (100000);
	return fd;
fail:
	close (fd);
	return -1;
}

static void
emit_tablet_mode (gint fd, gint tablet)
{
	struct input_event ev[2];

	memset (ev, 0, sizeof (ev));
	ev[0].type  = EV_SW;
	ev[0].code  = SW_TABLET_MODE;
	ev[0].value = tablet;
	ev[1].type  = EV_SYN;
	ev[1].code  = SYN_REPORT;

	if (write (fd, ev, sizeof (ev)) < 0) { /* ignore */ }
}

static float
median3 (float a, float b, float c)
{
	if (a > b) { float t = a; a = b; b = t; }
	if (b > c) { float t = b; b = c; c = t; }
	if (a > b) { float t = a; a = b; b = t; }
	return b;
}

static float
median3_step (MedianFilter *f, float v)
{
	if (f->count < 3) {
		f->buf[f->count++] = v;
		return v;
	}

	f->buf[0] = f->buf[1];
	f->buf[1] = f->buf[2];
	f->buf[2] = v;
	return median3 (f->buf[0], f->buf[1], f->buf[2]);
}

static float
sample_variance (const float *buf, gint n)
{
	float sum = 0.0f, sum_sq = 0.0f;
	float mean;

	if (n <= 1)
		return 0.0f;

	for (gint i = 0; i < n; i++) {
		sum += buf[i];
		sum_sq += buf[i] * buf[i];
	}
	mean = sum / (float) n;
	return (sum_sq - (float) n * mean * mean) / (float) (n - 1);
}

static void
median_filter_z (OrientState *s, float *z)
{
	if (s->z_median_count < 3) {
		s->z_median_buf[s->z_median_count++] = *z;
		return;
	}

	s->z_median_buf[0] = s->z_median_buf[1];
	s->z_median_buf[1] = s->z_median_buf[2];
	s->z_median_buf[2] = *z;
	*z = median3 (s->z_median_buf[0], s->z_median_buf[1], s->z_median_buf[2]);
}

static gboolean
store_in_buffer (OrientState *s, const float in[3])
{
	gint idx = s->buf_idx;

	s->buf_x[idx] = in[0];
	s->buf_y[idx] = in[1];
	s->buf_z[idx] = in[2];
	s->buf_idx++;
	if (s->buf_idx >= ORIENT_BUF_SIZE) {
		s->buf_idx = 0;
		s->buf_full = 1;
	}
	return s->buf_full;
}

static float
max_buffer_variance (OrientState *s)
{
	float var_x = sample_variance (s->buf_x, ORIENT_BUF_SIZE);
	float var_y = sample_variance (s->buf_y, ORIENT_BUF_SIZE);
	float var_z = sample_variance (s->buf_z, ORIENT_BUF_SIZE);
	float mv = var_x;

	if (var_y > mv) mv = var_y;
	if (var_z > mv) mv = var_z;
	return mv;
}

/*
 * Gate orientation classification until the input has been quiet and
 * stable. Stays TRUE afterwards regardless of motion; only a large Z
 * jump suppresses classification for a single sample (it would otherwise
 * reset the debounce of the true orientation).
 */
static gboolean
orientation_filter_settled (OrientState *s, const Vec3 *accel)
{
	float in[3] = { accel->x, accel->y, accel->z };
	float max_var;
	float avg_z = 0.0f;

	median_filter_z (s, &in[2]);

	if (!store_in_buffer (s, in))
		return FALSE;

	max_var = max_buffer_variance (s);
	if (max_var > ORIENT_VARIANCE_THRESH)
		s->quiet_count = 0;
	else if (s->quiet_count < ORIENT_STABLE_MIN)
		s->quiet_count++;

	if (s->quiet_count < ORIENT_STABLE_MIN)
		return s->primed;

	for (gint i = 0; i < ORIENT_BUF_SIZE; i++)
		avg_z += s->buf_z[i];
	avg_z /= (float) ORIENT_BUF_SIZE;

	if (!s->have_ref) {
		s->ref_z = avg_z;
		s->have_ref = TRUE;
	} else if (fabsf (avg_z - s->ref_z) > ORIENT_JUMP_LIMIT) {
		s->have_ref = FALSE;
		return FALSE;
	} else {
		s->ref_z = avg_z;
	}

	s->primed = TRUE;
	return TRUE;
}

static gint
classify_orientation (const Vec3 *accel)
{
	float abs_x = fabsf (accel->x);
	float abs_y = fabsf (accel->y);

	/* Need at least 0.4g of tilt to determine orientation */
	if (abs_x < 0.4f && abs_y < 0.4f)
		return -1;

	if (abs_y >= abs_x)
		return accel->y < 0.0f ? MXC_ORIENT_NORMAL : MXC_ORIENT_INVERTED;
	else
		return accel->x > 0.0f ? MXC_ORIENT_RIGHT : MXC_ORIENT_LEFT;
}

static float
compute_hinge_angle (DrvData *drv_data, const Vec3 *cal1, const Vec3 *cal2)
{
	/* Negate accel1 Y and Z per DSDT calibration convention */
	float a1x = cal1->x, a1y = -(cal1->y), a1z = -(cal1->z);
	float a2x = cal2->x, a2y = cal2->y,     a2z = cal2->z;
	float mag1, mag2;
	float v1, v2;
	float ang1, ang2, diff;

	mag1 = sqrtf (a1x * a1x + a1y * a1y + a1z * a1z);
	mag2 = sqrtf (a2x * a2x + a2y * a2y + a2z * a2z);

	if (fabsf (mag1) > 1e-05f) {
		a1x /= mag1; a1y /= mag1; a1z /= mag1;
	}
	if (fabsf (mag2) > 1e-05f) {
		a2x /= mag2; a2y /= mag2; a2z /= mag2;
	}

	if (fabsf (a1z) < 1e-05f)
		a1z = drv_data->prev_z1;
	if (fabsf (a2z) < 1e-05f)
		a2z = drv_data->prev_z2;
	drv_data->prev_z1 = a1z;
	drv_data->prev_z2 = a2z;

	/* The GMTR axis-mode nibble is 3 (negated X with the display in the
	 * Windows driver's slot 1); +X with the base in our slot 1 yields the
	 * same hinge angle. */
	v1 = a1x;
	v2 = a2x;

	/* DSDT uses 180/3.14 (not M_PI) */
	ang1 = atan2f (v1, a1z) * (180.0f / 3.14f);
	ang2 = atan2f (v2, a2z) * (180.0f / 3.14f);

	if (ang1 < 0.0f) ang1 += 360.0f;
	if (ang2 < 0.0f) ang2 += 360.0f;

	diff = ang1 - ang2;
	if (ang1 < ang2) diff += 360.0f;

	return diff;
}

/*
 * Feed values that make orientation_calc() return the desired result.
 * Scale is set so the SCALE() macro in orientation.c produces the
 * raw value back (scale * 256 / 9.81 = 1).
 */
static void
build_synthetic_readings (gint orient, AccelReadings *readings)
{
	set_accel_scale (&readings->scale, 9.81 / 256.0);

	switch (orient) {
	case MXC_ORIENT_NORMAL:
		readings->accel_x = 0;
		readings->accel_y = -256;
		readings->accel_z = 0;
		break;
	case MXC_ORIENT_INVERTED:
		readings->accel_x = 0;
		readings->accel_y = 256;
		readings->accel_z = 0;
		break;
	case MXC_ORIENT_LEFT:
		readings->accel_x = 256;
		readings->accel_y = 0;
		readings->accel_z = 0;
		break;
	case MXC_ORIENT_RIGHT:
		readings->accel_x = -256;
		readings->accel_y = 0;
		readings->accel_z = 0;
		break;
	default:
		readings->accel_x = 0;
		readings->accel_y = -256;
		readings->accel_z = 0;
		break;
	}
}

static void
recover_i2c (DrvData *drv_data)
{
	gchar *controllers[2] = { NULL, NULL };

	g_debug ("I2C read failed, attempting re-unbind");
	for (gint i = 0; i < 2; i++) {
		if (drv_data->i2c_fds[i] >= 0) {
			close (drv_data->i2c_fds[i]);
			drv_data->i2c_fds[i] = -1;
		}
	}
	try_unbind ();
	if (find_accels (drv_data->i2c_fds, controllers) < 2) {
		g_warning ("Failed to re-open MXC6655 accelerometers");
		drv_data->i2c_fds[0] = drv_data->i2c_fds[1] = -1;
	} else if (drv_data->rotation_controller != NULL) {
		gint idx = match_controller (drv_data->rotation_controller, controllers);

		if (idx >= 0) {
			drv_data->rotation_idx = idx;
		} else {
			g_warning ("Rotation sensor lost after I2C recovery");
			drv_data->rotation_idx = 0;
		}
	}
	g_free (controllers[0]);
	g_free (controllers[1]);
}

static void
update_orientation_debounce (SensorDevice *sensor_device, DrvData *drv_data,
			     const Vec3 *accel)
{
	gint new_orient;

	if (!orientation_filter_settled (&drv_data->orient, accel))
		return;

	new_orient = classify_orientation (accel);
	if (new_orient < 0)
		return;

	if (drv_data->mode != 1)
		new_orient = MXC_ORIENT_RIGHT;

	new_orient = compose_orientation (new_orient, drv_data->panel_deg);

	if (new_orient != drv_data->pending_orient) {
		drv_data->pending_orient = new_orient;
		drv_data->orient_debounce = (new_orient != drv_data->cur_orient) ? 1 : 0;
		return;
	}

	if (new_orient == drv_data->cur_orient)
		return;

	drv_data->orient_debounce++;
	if (drv_data->orient_debounce > drv_data->debounce) {
		AccelReadings readings;

		drv_data->cur_orient = new_orient;
		drv_data->orient_debounce = 0;
		g_debug ("Orientation changed to %d (panel_deg=%d)",
			 new_orient, drv_data->panel_deg);

		build_synthetic_readings (new_orient, &readings);
		sensor_device->callback_func (sensor_device,
					      (gpointer) &readings,
					      sensor_device->user_data);
	}
}

static void emit_orientation (SensorDevice *sensor_device, gint orient);

/* GNOME rotates only when a reading arrives, and a near-upright display after
 * unfolding produces none -- so emit landscape rather than wait for the
 * accelerometer classifier. */
static void
force_landscape (SensorDevice *sensor_device)
{
	DrvData *drv_data = (DrvData *) sensor_device->priv;
	gint orient = compose_orientation (MXC_ORIENT_RIGHT, drv_data->panel_deg);

	drv_data->cur_orient = orient;
	drv_data->pending_orient = orient;
	drv_data->orient_debounce = 0;
	emit_orientation (sensor_device, orient);
}

static void
set_tablet_mode (SensorDevice *sensor_device, gint mode, float angle)
{
	DrvData *drv_data = (DrvData *) sensor_device->priv;

	drv_data->mode = mode;
	g_debug ("%s mode (angle=%.1f)", mode ? "Tablet" : "Laptop", angle);
	call_ltsm (mode, &drv_data->ltsm_warned);
	if (drv_data->uinput_fd >= 0)
		emit_tablet_mode (drv_data->uinput_fd, mode);

	if (mode == 0)
		force_landscape (sensor_device);
}

static void
update_tablet_mode (SensorDevice *sensor_device, float angle)
{
	DrvData *drv_data = (DrvData *) sensor_device->priv;

	if (angle > drv_data->tablet_thresh) {
		drv_data->t_count++;
		drv_data->l_count = 0;
		drv_data->n_count = 0;
		if (drv_data->mode != 1 && drv_data->t_count > drv_data->debounce)
			set_tablet_mode (sensor_device, 1, angle);
		return;
	}

	drv_data->t_count = 0;
	if (angle >= drv_data->laptop_thresh) {
		drv_data->n_count++;
		drv_data->l_count = 0;
		return;
	}

	/* Count below the minimum angle too, as the Windows driver does; the
	 * transition itself stays gated on reopening past it. */
	drv_data->l_count++;
	drv_data->n_count = 0;
	if (drv_data->mode != 0 && drv_data->l_count > drv_data->debounce &&
	    angle > drv_data->min_angle)
		set_tablet_mode (sensor_device, 0, angle);
}

static gboolean
poll_sensors (gpointer user_data)
{
	SensorDevice *sensor_device = user_data;
	DrvData *drv_data = (DrvData *) sensor_device->priv;
	Vec3 raw1, raw2, a1, a2;
	float angle;

	if (read_accel (drv_data->i2c_fds[0], &raw1) < 0 ||
	    read_accel (drv_data->i2c_fds[1], &raw2) < 0) {
		recover_i2c (drv_data);
		return G_SOURCE_CONTINUE;
	}

	a1 = calibrate (&raw1, drv_data->cal1);
	a2 = calibrate (&raw2, drv_data->cal2);

	{
		Vec3 orient = drv_data->rotation_idx == 0 ? raw1 : raw2;
		orient.x = -orient.x;
		update_orientation_debounce (sensor_device, drv_data, &orient);
	}

	/* Suppress single-sample spikes on the Z axis both angle inputs use */
	a1.z = median3_step (&drv_data->hinge_median[0], a1.z);
	a2.z = median3_step (&drv_data->hinge_median[1], a2.z);

	/* Both sensors' Y axes point along the hinge, so gravity on Y means the
	 * hinge is far from horizontal and the X-Z projection the angle is
	 * computed from is no longer meaningful. Windows abstains unless both
	 * stay below 0.9 (~64 degrees of hinge tilt). */
	if (fabsf (a1.y) > HINGE_AXIS_MAX || fabsf (a2.y) > HINGE_AXIS_MAX)
		return G_SOURCE_CONTINUE;

	angle = compute_hinge_angle (drv_data, &a1, &a2);
	g_debug ("Hinge angle: %.1f  mode=%d", angle, drv_data->mode);
	update_tablet_mode (sensor_device, angle);

	return G_SOURCE_CONTINUE;
}

static gboolean
mxc6655_discover (GUdevDevice *device)
{
	const char *path;

	path = g_udev_device_get_sysfs_path (device);
	if (!path || !strstr (path, ACPI_MATCH_ID))
		return FALSE;

	g_debug ("Found MXC6655 dual-accel at %s", path);
	return TRUE;
}

static void setup_lid_watch (SensorDevice *sensor_device);

static SensorDevice *
mxc6655_open (GUdevDevice *device)
{
	SensorDevice *sensor_device;
	DrvData *drv_data;
	gchar *controllers[2] = { NULL, NULL };
	gint found;

	/* Try to find both accelerometers, unbinding kernel driver if needed */
	drv_data = g_new0 (DrvData, 1);
	if (!drv_data)
		return NULL;
	drv_data->i2c_fds[0] = -1;
	drv_data->i2c_fds[1] = -1;
	drv_data->uinput_fd = -1;
	drv_data->lid_fd = -1;
	drv_data->mode = -1;
	drv_data->cur_orient = -1;
	drv_data->pending_orient = -1;
	drv_data->prev_z1 = 1.0f;
	drv_data->prev_z2 = 1.0f;
	drv_data->panel_deg = detect_panel_rotation ();
	drv_data->rotation_idx = 0;

	/* DSDT GMTR calibration matrices (defaults) */
	memcpy (drv_data->cal1, (gint8[]){ 1, 0, 0, 0, 1, 0, 0, 0, 1 }, 9);
	memcpy (drv_data->cal2, (gint8[]){ 1, 0, 0, 0, -1, 0, 0, 0, -1 }, 9);

	/* Try to load matrices and thresholds from ACPI DSDT */
	if (load_gmtr (drv_data))
		g_debug ("Dynamic calibration loaded from ACPI");
	else
		g_debug ("Using default calibration matrices");

	found = find_accels (drv_data->i2c_fds, controllers);
	if (found < 2) {
		g_debug ("Found %d accelerometers, trying unbind", found);
		for (gint i = 0; i < found; i++) {
			close (drv_data->i2c_fds[i]);
			drv_data->i2c_fds[i] = -1;
		}
		g_free (controllers[0]);
		g_free (controllers[1]);
		controllers[0] = controllers[1] = NULL;
		try_unbind ();
		found = find_accels (drv_data->i2c_fds, controllers);
	}

	if (found < 2) {
		g_debug ("Need 2 MXC6655 accelerometers, found %d", found);
		for (gint i = 0; i < found; i++)
			close (drv_data->i2c_fds[i]);
		g_free (controllers[0]);
		g_free (controllers[1]);
		g_free (drv_data);
		return NULL;
	}

	resolve_rotation_sensor (drv_data, controllers);
	g_free (controllers[0]);
	g_free (controllers[1]);

	/* Setup uinput for SW_TABLET_MODE */
	drv_data->uinput_fd = setup_uinput ();
	if (drv_data->uinput_fd < 0)
		g_warning ("Cannot create uinput device for SW_TABLET_MODE");
	else
		g_debug ("Created uinput device for SW_TABLET_MODE");

	sensor_device = g_new0 (SensorDevice, 1);
	sensor_device->name = g_strdup ("MXC6655 dual-accel");
	sensor_device->priv = drv_data;

	setup_lid_watch (sensor_device);

	return sensor_device;
}

static void
emit_orientation (SensorDevice *sensor_device, gint orient)
{
	AccelReadings readings;

	build_synthetic_readings (orient, &readings);
	sensor_device->callback_func (sensor_device,
				      (gpointer) &readings,
				      sensor_device->user_data);
}

static void
send_current_reading (SensorDevice *sensor_device)
{
	DrvData *drv_data = (DrvData *) sensor_device->priv;
	gint orient = drv_data->cur_orient;

	if (orient < 0)
		orient = compose_orientation (MXC_ORIENT_RIGHT, drv_data->panel_deg);

	emit_orientation (sensor_device, orient);
}

static gboolean
polling_wanted (DrvData *drv_data)
{
	return drv_data->want_polling && !drv_data->lid_closed;
}

static void
start_poll_timer (SensorDevice *sensor_device)
{
	DrvData *drv_data = (DrvData *) sensor_device->priv;

	drv_data->timeout_id = g_timeout_add (drv_data->poll_ms, poll_sensors, sensor_device);
	g_source_set_name_by_id (drv_data->timeout_id, "[mxc6655] poll_sensors");
	g_message ("MXC6655 dual-accel: polling active");
}

static void
stop_poll_timer (SensorDevice *sensor_device)
{
	DrvData *drv_data = (DrvData *) sensor_device->priv;
	const char *reason;

	g_source_remove (drv_data->timeout_id);
	drv_data->timeout_id = 0;

	if (drv_data->lid_closed)
		reason = "lid closed";
	else
		reason = "no client";
	g_message ("MXC6655 dual-accel: polling idle (%s)", reason);
}

static void
update_polling_timer (SensorDevice *sensor_device)
{
	DrvData *drv_data = (DrvData *) sensor_device->priv;
	gboolean wanted = polling_wanted (drv_data);

	if (wanted && drv_data->timeout_id == 0)
		start_poll_timer (sensor_device);
	else if (!wanted && drv_data->timeout_id > 0)
		stop_poll_timer (sensor_device);
}

static gint
open_lid_switch (void)
{
	for (gint i = 0; i < MAX_INPUT_DEV; i++) {
		char path[64];
		char name[256] = { 0 };
		gint fd;

		g_snprintf (path, sizeof (path), "/dev/input/event%d", i);
		fd = open (path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			continue;

		if (ioctl (fd, EVIOCGNAME (sizeof (name)), name) >= 0 &&
		    strcmp (name, LID_SWITCH_NAME) == 0)
			return fd;

		close (fd);
	}

	return -1;
}

static gboolean
read_lid_closed (gint fd)
{
	unsigned long bits = 0;

	if (ioctl (fd, EVIOCGSW (sizeof (bits)), &bits) < 0)
		return FALSE;

	return (bits & (1UL << SW_LID)) != 0;
}

static void
set_lid_closed (SensorDevice *sensor_device,
		gboolean      closed)
{
	DrvData *drv_data = (DrvData *) sensor_device->priv;

	if (drv_data->lid_closed == closed)
		return;

	drv_data->lid_closed = closed;
	update_polling_timer (sensor_device);
}

static gboolean
lid_event_cb (GIOChannel   *channel,
	      GIOCondition  condition,
	      gpointer      user_data)
{
	SensorDevice *sensor_device = user_data;
	DrvData *drv_data = (DrvData *) sensor_device->priv;
	struct input_event ev;

	if (condition & (G_IO_HUP | G_IO_ERR)) {
		drv_data->lid_watch_id = 0;
		return G_SOURCE_REMOVE;
	}

	while (TRUE) {
		gsize bytes_read;
		GIOStatus status;

		status = g_io_channel_read_chars (channel, (gchar *) &ev,
						  sizeof (ev), &bytes_read, NULL);
		if (status != G_IO_STATUS_NORMAL || bytes_read != sizeof (ev))
			break;
		if (ev.type != EV_SW || ev.code != SW_LID)
			continue;

		set_lid_closed (sensor_device, ev.value != 0);
	}

	return G_SOURCE_CONTINUE;
}

static void
setup_lid_watch (SensorDevice *sensor_device)
{
	DrvData *drv_data = (DrvData *) sensor_device->priv;
	GIOChannel *channel;

	drv_data->lid_fd = open_lid_switch ();
	if (drv_data->lid_fd < 0) {
		g_debug ("No lid switch found, polling not lid-gated");
		return;
	}

	drv_data->lid_closed = read_lid_closed (drv_data->lid_fd);

	channel = g_io_channel_unix_new (drv_data->lid_fd);
	g_io_channel_set_encoding (channel, NULL, NULL);
	g_io_channel_set_buffered (channel, FALSE);
	drv_data->lid_watch_id = g_io_add_watch (channel,
						 G_IO_IN | G_IO_HUP | G_IO_ERR,
						 lid_event_cb, sensor_device);
	g_io_channel_unref (channel);
}

static void
mxc6655_set_polling (SensorDevice *sensor_device,
		     gboolean      state)
{
	DrvData *drv_data = (DrvData *) sensor_device->priv;

	/* Without --lazy the timer already runs (want_polling == state), yet the
	 * proxy holds each Claim reply until a reading is emitted -- so always send
	 * one, not just on a state change. */
	if (state)
		send_current_reading (sensor_device);

	if (drv_data->want_polling == state)
		return;
	drv_data->want_polling = state;

	update_polling_timer (sensor_device);
}

static void
mxc6655_close (SensorDevice *sensor_device)
{
	DrvData *drv_data = (DrvData *) sensor_device->priv;

	if (drv_data->lid_watch_id > 0)
		g_source_remove (drv_data->lid_watch_id);
	if (drv_data->lid_fd >= 0)
		close (drv_data->lid_fd);

	if (drv_data->uinput_fd >= 0) {
		ioctl (drv_data->uinput_fd, UI_DEV_DESTROY);
		close (drv_data->uinput_fd);
	}

	for (gint i = 0; i < 2; i++) {
		if (drv_data->i2c_fds[i] >= 0)
			close (drv_data->i2c_fds[i]);
	}

	g_clear_pointer (&drv_data->rotation_controller, g_free);
	g_clear_pointer (&sensor_device->priv, g_free);
	g_free (sensor_device);
}

SensorDriver mxc6655_accel = {
	.driver_name = "MXC6655 dual accelerometer",
	.type = DRIVER_TYPE_ACCEL,

	.discover = mxc6655_discover,
	.open = mxc6655_open,
	.set_polling = mxc6655_set_polling,
	.close = mxc6655_close,
};
