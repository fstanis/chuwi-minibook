/*
 * MXC6655 dual-accelerometer driver for CHUWI MiniBook.
 * Reads two MXC6655 accelerometers via raw I2C, computes hinge angle
 * for tablet mode detection, and provides orientation via a multi-stage
 * filter pipeline. Tablet mode transitions are emitted via uinput
 * (SW_TABLET_MODE) and ACPI.
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

/* GMTR PARB thresholds from DSDT \_SB.ACMK.GMTR */
#define GMTR_TABLET_THRESH	185.0f
#define GMTR_LAPTOP_THRESH	175.0f
#define GMTR_MIN_ANGLE		30.0f
#define GMTR_DEBOUNCE		5
#define GMTR_POLL_MS		50

#define GRAVITY_MIN		0.3f

/* Multi-stage orientation filter constants */
#define ORIENT_BUF_SIZE		20
#define ORIENT_OFFSET_BUF	5
#define ORIENT_VARIANCE_THRESH	0.01f
#define ORIENT_STABLE_MIN	10
#define ORIENT_EMA_ALPHA	0.01f
#define ORIENT_EMA_DECAY	0.99f
#define ORIENT_DRIFT_LIMIT	0.2f
#define ORIENT_JUMP_LIMIT	2.4f
#define ORIENT_OUTLIER_RANGE	0.5f
#define ORIENT_MAG_LO		0.85f
#define ORIENT_MAG_HI		1.15f

typedef struct {
	float x, y, z;
} Vec3;

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

	/* EMA filters per axis [x=0, y=1, z=2] */
	float              ema_delta[3];
	float              ema_abs_delta[3];
	float              smoothed[3];

	/* Circular buffer for stability detection */
	float              buf_x[ORIENT_BUF_SIZE];
	float              buf_y[ORIENT_BUF_SIZE];
	float              buf_z[ORIENT_BUF_SIZE];
	gint               buf_idx;
	gint               buf_full;

	/* Accumulator for long-term stability */
	gint               stable_count;
	float              acc_x, acc_y, acc_z;
	float              acc_sq_x, acc_sq_y, acc_sq_z;
	float              peak_x, peak_y, peak_z;
	float              trough_x, trough_y, trough_z;
	gint               outlier_detected;

	/* Reference gravity point */
	float              ref_x, ref_y, ref_z;

	/* Z direction: 1=positive, -1=negative, 0=unknown */
	gint               z_dir;

	/* Gravity offset tracking */
	float              gravity_offset;
	float              trimmed_mean;

	/* Offset buffer (trimmed mean) */
	float              offset_buf[ORIENT_OFFSET_BUF];
	gint               offset_count;
	gint               offset_init;

	/* Output */
	float              out_offset;
	float              out_mean;
	float              out_z;
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

	/* Tablet mode state machine */
	gint               mode;
	gint               t_count;
	gint               l_count;
	gint               n_count;

	/* Dynamic thresholds from DSDT GMTR */
	float              tablet_thresh;
	float              laptop_thresh;
	float              min_angle;

	/* Orientation state */
	OrientState        orient;
	gint               cur_orient;
	gint               orient_debounce;
	gint               pending_orient;
} DrvData;

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
find_accels (gint fds[2])
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
			fds[found++] = fd;
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

static gint
parse_acpi_package (const gchar *buf, guint64 *values, gint max_values)
{
	gchar **elements;
	gchar *end;
	g_autofree gchar *copy = NULL;
	gint count = 0;

	if (buf[0] != '{')
		return 0;

	copy = g_strdup (buf + 1);
	end = strchr (copy, '}');
	if (end)
		*end = '\0';

	elements = g_strsplit (copy, ", ", -1);
	for (gint i = 0; elements[i] != NULL && count < max_values; i++) {
		if (strlen (elements[i]) == 0)
			continue;
		values[count++] = g_ascii_strtoull (elements[i], NULL, 0);
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

/*
 * Reconstruct Z component with smoothing in transition zone.
 * z_sq = 1 - x² - y² (clamped >= 0)
 * Interpolates between scaled and sqrt regions to avoid discontinuity.
 */
static float
reconstruct_z (float raw_z, float z_sq, float z_hint)
{
	float val;
	float z_sq_c = z_sq < 0.0f ? 0.0f : z_sq;
	double d = (double) z_sq_c;
	float frac;

	if (d <= 0.09) {
		val = (float) (z_hint <= 0.0f ? -d * 0.5 : d * 0.5);
	} else if (d > 0.16) {
		double s = sqrt (d);
		val = (float) (z_hint <= 0.0f ? -s : s);
	} else {
		double s16 = sqrt (0.16);
		double interp = (d - 0.09) * s16 + (0.16 - d) * 0.09 * 0.5;
		interp = z_hint <= 0.0f ? -interp : interp;
		val = (float) (interp / 0.07);
	}

	frac = (raw_z * 100.0f - (float) (gint) (raw_z * 100.0f)) / 100.0f;
	return val + frac;
}

/*
 * Resolve gravity offset from two reference points.
 * Computes 4 candidate Z offsets (±sqrt for each), picks the closest
 * pair, validates gravity magnitude, and determines Z direction sign.
 */
static gint
resolve_gravity_offset (const float ref[3], const float cur[3],
			float trimmed_mean, gint *z_dir, float *out_offset)
{
	float ref_x2 = ref[0] * ref[0];
	float ref_y2 = ref[1] * ref[1];
	float cur_x2 = cur[0] * cur[0];
	float cur_y2 = cur[1] * cur[1];

	double ref_zsq = (1.0 - (double) ref_x2) - (double) ref_y2;
	double cur_zsq = (1.0 - (double) cur_x2) - (double) cur_y2;
	float ref_zp, ref_zm, cur_zp, cur_zm;
	float d_mm, d_pm, d_pp, d_mp;
	float ref_span, cur_span, threshold;
	float offset;
	float ref_mag, cur_mag;
	gint new_dir;

	if (ref_zsq < 0.0) ref_zsq = 0.0;
	if (cur_zsq < 0.0) cur_zsq = 0.0;

	ref_zp = (float) (ref[2] + sqrt (ref_zsq));
	ref_zm = (float) (ref[2] - sqrt (ref_zsq));
	cur_zp = (float) (cur[2] + sqrt (cur_zsq));
	cur_zm = (float) (cur[2] - sqrt (cur_zsq));

	d_mm = fabsf (ref_zm - cur_zm);
	d_pm = fabsf (ref_zp - cur_zm);
	d_pp = fabsf (ref_zp - cur_zp);
	d_mp = fabsf (ref_zm - cur_zp);

	ref_span = fabsf (ref_zm - ref_zp);
	cur_span = fabsf (cur_zm - cur_zp);
	threshold = (cur_span + ref_span) * 0.08f;

	if (d_mm <= d_pm && d_mm <= d_pp && d_mm <= d_mp && d_mm < threshold) {
		offset = (cur_zm + ref_zm) * 0.5f;
		new_dir = 1;
	} else if (d_pm <= d_mm && d_pm <= d_pp && d_pm <= d_mp && d_pm < threshold) {
		offset = (cur_zm + ref_zp) * 0.5f;
		new_dir = 1;
	} else if (d_pp <= d_mm && d_pp <= d_pm && d_pp <= d_mp && d_pp < threshold) {
		offset = (cur_zp + ref_zp) * 0.5f;
		new_dir = -1;
	} else if (d_mp <= d_mm && d_mp <= d_pm && d_mp <= d_pp && d_mp < threshold) {
		offset = (cur_zp + ref_zm) * 0.5f;
		new_dir = -1;
	} else {
		offset = (cur_zp + ref_zp) * 0.5f;
		new_dir = -1;
	}

	ref_mag = (ref[2] - offset) * (ref[2] - offset) + ref_y2 + ref_x2;
	cur_mag = (cur[2] - offset) * (cur[2] - offset) + cur_y2 + cur_x2;
	if (ref_mag <= ORIENT_MAG_LO || ref_mag >= ORIENT_MAG_HI ||
	    cur_mag <= ORIENT_MAG_LO || cur_mag >= ORIENT_MAG_HI)
		return 0;

	*z_dir = new_dir;

	if (trimmed_mean != 0.0f && fabsf (offset - trimmed_mean) > 1.0f) {
		*out_offset = 0.0f;
		return 0;
	}

	*out_offset = offset;
	return 1;
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

static void
ema_smooth (OrientState *s, const float in[3])
{
	for (gint i = 0; i < 3; i++) {
		float delta = in[i] - s->smoothed[i];
		float abs_delta = fabsf (delta);
		float weight;

		s->ema_delta[i] = s->ema_delta[i] * ORIENT_EMA_DECAY + delta * ORIENT_EMA_ALPHA;
		s->ema_abs_delta[i] = s->ema_abs_delta[i] * ORIENT_EMA_DECAY + abs_delta * ORIENT_EMA_ALPHA;

		if (fabsf (s->ema_abs_delta[i]) >= 0.0001f)
			weight = fabsf (s->ema_delta[i] / s->ema_abs_delta[i]);
		else
			weight = 0.5f;

		s->smoothed[i] = (1.0f - weight) * s->smoothed[i] + weight * in[i];
	}
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

static void
reset_accumulator (OrientState *s)
{
	s->acc_x = s->acc_y = s->acc_z = 0.0f;
	s->acc_sq_x = s->acc_sq_y = s->acc_sq_z = 0.0f;
	s->stable_count = 0;
	s->outlier_detected = 0;
}

static void
average_buffer (OrientState *s, float *avg_x, float *avg_y, float *avg_z)
{
	float sx = 0.0f, sy = 0.0f, sz = 0.0f;

	for (gint i = 0; i < ORIENT_BUF_SIZE; i++) {
		sx += s->buf_x[i];
		sy += s->buf_y[i];
		sz += s->buf_z[i];
	}
	*avg_x = sx / (float) ORIENT_BUF_SIZE;
	*avg_y = sy / (float) ORIENT_BUF_SIZE;
	*avg_z = sz / (float) ORIENT_BUF_SIZE;
}

static float
signed_z (float z_sq, gint z_dir)
{
	if (z_dir != -1)
		return sqrtf (z_sq);
	return -sqrtf (z_sq);
}

static void
record_offset (OrientState *s)
{
	if (s->offset_count < ORIENT_OFFSET_BUF)
		s->offset_buf[s->offset_count++] = s->gravity_offset;
}

static void
try_resolve_offset (OrientState *s,
		    const float ref[3], const float cur[3])
{
	float new_offset;
	gint ret;

	ret = resolve_gravity_offset (ref, cur, s->trimmed_mean,
				      &s->z_dir, &new_offset);
	if (ret == 1) {
		s->gravity_offset = new_offset;
		record_offset (s);
	}
	reset_accumulator (s);
}

static void
bootstrap_reference (OrientState *s, float avg_x, float avg_y,
		     float avg_z, float z_sq)
{
	s->ref_x = avg_x;
	s->ref_y = avg_y;
	s->ref_z = avg_z;
	s->gravity_offset = avg_z - signed_z (z_sq, s->z_dir);
}

static void
accumulate_sample (OrientState *s, float avg_x, float avg_y, float avg_z)
{
	s->stable_count++;
	s->acc_x += avg_x;
	s->acc_y += avg_y;
	s->acc_z += avg_z;
	s->acc_sq_x += avg_x * avg_x;
	s->acc_sq_y += avg_y * avg_y;
	s->acc_sq_z += avg_z * avg_z;
}

static void
update_peak_trough (OrientState *s, float avg_x, float avg_y, float avg_z)
{
	if (s->stable_count == 1) {
		s->peak_x = s->trough_x = avg_x;
		s->peak_y = s->trough_y = avg_y;
		s->peak_z = s->trough_z = avg_z;
	}
	if (avg_z > s->peak_z) {
		s->peak_x = avg_x;
		s->peak_y = avg_y;
		s->peak_z = avg_z;
	}
	if (avg_z < s->trough_z) {
		s->trough_x = avg_x;
		s->trough_y = avg_y;
		s->trough_z = avg_z;
	}
}

static void
detect_outlier (OrientState *s, float avg_z)
{
	if (avg_z == s->trough_z &&
	    s->peak_z - avg_z > ORIENT_OUTLIER_RANGE)
		s->outlier_detected = 1;
	if (avg_z == s->peak_z &&
	    avg_z - s->trough_z > ORIENT_OUTLIER_RANGE)
		s->outlier_detected = 1;
}

static void
handle_long_stable (OrientState *s, float avg_z, float z_sq)
{
	float v_x = (s->acc_sq_x * 1000.0f - s->acc_x * s->acc_x) / 999000.0f;
	float v_y = (s->acc_sq_y * 1000.0f - s->acc_y * s->acc_y) / 999000.0f;
	float v_z = (s->acc_sq_z * 1000.0f - s->acc_z * s->acc_z) / 999000.0f;
	float mv = v_x;

	if (v_y > mv) mv = v_y;
	if (v_z > mv) mv = v_z;

	if (mv < 0.004f) {
		s->gravity_offset = avg_z - signed_z (z_sq, s->z_dir);
		record_offset (s);
	}
}

static void
handle_stable_drift (OrientState *s, float avg_x, float avg_y,
		     float avg_z, float z_sq)
{
	accumulate_sample (s, avg_x, avg_y, avg_z);
	update_peak_trough (s, avg_x, avg_y, avg_z);

	if (s->stable_count < 2)
		return;

	detect_outlier (s, avg_z);

	if (s->outlier_detected) {
		float ref_pt[3] = { s->peak_x, s->peak_y, s->peak_z };
		float cur_pt[3] = { avg_x, avg_y, avg_z };
		try_resolve_offset (s, ref_pt, cur_pt);
	} else if (s->stable_count >= 1000) {
		handle_long_stable (s, avg_z, z_sq);
	}
}

static void
update_offset_trimmed_mean (OrientState *s)
{
	float mn, mx, sum;

	if (s->offset_count < ORIENT_OFFSET_BUF)
		return;

	s->offset_count = 0;
	mn = mx = s->offset_buf[0];
	sum = 0.0f;
	for (gint i = 0; i < ORIENT_OFFSET_BUF; i++) {
		sum += s->offset_buf[i];
		if (s->offset_buf[i] > mx) mx = s->offset_buf[i];
		if (s->offset_buf[i] < mn) mn = s->offset_buf[i];
	}
	s->trimmed_mean = (sum - mn - mx) / (float) (ORIENT_OFFSET_BUF - 2);
}

static gint
update_gravity_tracking (OrientState *s, float avg_x, float avg_y,
			 float avg_z, float z_sq, float *mean_offset)
{
	float z_drift;

	if (s->ref_x == 0.0f && s->ref_y == 0.0f && s->ref_z == 0.0f) {
		bootstrap_reference (s, avg_x, avg_y, avg_z, z_sq);
		return 0;
	}

	z_drift = fabsf (avg_z - s->ref_z);

	if (z_drift <= ORIENT_DRIFT_LIMIT) {
		handle_stable_drift (s, avg_x, avg_y, avg_z, z_sq);
	} else if (z_drift > ORIENT_JUMP_LIMIT) {
		s->ref_x = s->ref_y = s->ref_z = 0.0f;
		return -1;
	} else {
		float ref_pt[3] = { s->ref_x, s->ref_y, s->ref_z };
		float cur_pt[3] = { avg_x, avg_y, avg_z };
		try_resolve_offset (s, ref_pt, cur_pt);
	}

	update_offset_trimmed_mean (s);

	s->ref_x = avg_x;
	s->ref_y = avg_y;
	s->ref_z = avg_z;

	if (s->offset_init && s->offset_count > 0) {
		s->offset_init = 0;
		*mean_offset = s->offset_buf[0];
	}

	if (avg_z - s->gravity_offset > 0.0f)
		s->z_dir = 1;
	else if (avg_z - s->gravity_offset < 0.0f)
		s->z_dir = -1;

	return 0;
}

static float
blend_z_output (OrientState *s, const float in[3], float max_var)
{
	float z_sq = (1.0f - s->smoothed[0] * s->smoothed[0])
		   - s->smoothed[1] * s->smoothed[1];
	float z_hint = s->smoothed[2] - s->gravity_offset;
	float z_out;

	if (max_var < 0.0024f) {
		z_out = reconstruct_z (in[2], z_sq, z_hint);
	} else if (max_var >= 0.0056f) {
		z_out = in[2] - s->gravity_offset;
	} else {
		float z_recon = reconstruct_z (in[2], z_sq, z_hint);
		float z_raw = in[2] - s->gravity_offset;
		z_out = ((z_raw - z_recon) * max_var + z_recon * 0.0056f
			- z_raw * 0.0024f) / 0.0032f;
	}

	if (max_var < 0.002f) {
		float mag = sqrtf (in[0] * in[0] + in[1] * in[1] + z_out * z_out);
		if (mag < 0.6f || mag > 1.4f)
			z_out = 0.5f;
	}

	return z_out;
}

static gint
update_orientation (OrientState *s, const Vec3 *accel)
{
	float in[3] = { accel->x, accel->y, accel->z };
	float max_var;
	float mean_offset = 0.0f;

	median_filter_z (s, &in[2]);
	ema_smooth (s, in);

	if (!store_in_buffer (s, in)) {
		float z_sq = (1.0f - s->smoothed[0] * s->smoothed[0])
			   - s->smoothed[1] * s->smoothed[1];
		s->out_offset = s->gravity_offset;
		s->out_mean = 0.0f;
		s->out_z = z_sq >= 0.0f ? signed_z (z_sq, s->z_dir) : 0.0f;
		return 0;
	}

	max_var = max_buffer_variance (s);

	if (max_var > ORIENT_VARIANCE_THRESH) {
		reset_accumulator (s);
	} else if (s->stable_count <= ORIENT_STABLE_MIN) {
		s->stable_count++;
	}

	if (s->stable_count > ORIENT_STABLE_MIN) {
		float avg_x, avg_y, avg_z, z_sq, xy_sq;

		average_buffer (s, &avg_x, &avg_y, &avg_z);

		xy_sq = avg_x * avg_x + avg_y * avg_y;
		z_sq = 1.0f - xy_sq;

		if (z_sq < 0.0f) {
			double norm = sqrt ((double) xy_sq);
			avg_x = (float) ((double) avg_x / norm) * 0.9999f;
			avg_y = (float) ((double) avg_y / norm) * 0.9999f;
			z_sq = 0.0f;
			reset_accumulator (s);
		}

		if (update_gravity_tracking (s, avg_x, avg_y, avg_z, z_sq,
					     &mean_offset) < 0)
			return 0;
	}

	s->out_offset = s->gravity_offset;
	s->out_mean = mean_offset;
	s->out_z = blend_z_output (s, in, max_var);

	if (fabsf (s->gravity_offset) < 1e-6f)
		return 0;
	if (fabsf (mean_offset) > 1e-6f)
		return 2;
	return 1;
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

	/* GMTR axis_mode_2=1: use X component for atan2 */
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
	g_debug ("I2C read failed, attempting re-unbind");
	for (gint i = 0; i < 2; i++) {
		if (drv_data->i2c_fds[i] >= 0) {
			close (drv_data->i2c_fds[i]);
			drv_data->i2c_fds[i] = -1;
		}
	}
	try_unbind ();
	if (find_accels (drv_data->i2c_fds) < 2) {
		g_warning ("Failed to re-open MXC6655 accelerometers");
		drv_data->i2c_fds[0] = drv_data->i2c_fds[1] = -1;
	}
}

static void
update_orientation_debounce (SensorDevice *sensor_device, DrvData *drv_data,
			     const Vec3 *accel)
{
	gint ost = update_orientation (&drv_data->orient, accel);
	gint new_orient;

	if (ost <= 0)
		return;

	new_orient = classify_orientation (accel);
	if (new_orient < 0)
		return;

	if (drv_data->mode != 1)
		new_orient = MXC_ORIENT_RIGHT;

	if (new_orient != drv_data->pending_orient) {
		drv_data->pending_orient = new_orient;
		drv_data->orient_debounce = (new_orient != drv_data->cur_orient) ? 1 : 0;
		return;
	}

	if (new_orient == drv_data->cur_orient)
		return;

	drv_data->orient_debounce++;
	if (drv_data->orient_debounce > GMTR_DEBOUNCE) {
		AccelReadings readings;

		drv_data->cur_orient = new_orient;
		drv_data->orient_debounce = 0;
		g_debug ("Orientation changed to %d", new_orient);

		build_synthetic_readings (new_orient, &readings);
		sensor_device->callback_func (sensor_device,
					      (gpointer) &readings,
					      sensor_device->user_data);
	}
}

static void
set_tablet_mode (DrvData *drv_data, gint mode, float angle)
{
	drv_data->mode = mode;
	g_debug ("%s mode (angle=%.1f)", mode ? "Tablet" : "Laptop", angle);
	call_ltsm (mode, &drv_data->ltsm_warned);
	if (drv_data->uinput_fd >= 0)
		emit_tablet_mode (drv_data->uinput_fd, mode);
}

static void
update_tablet_mode (DrvData *drv_data, float angle)
{
	if (angle > drv_data->tablet_thresh) {
		drv_data->t_count++;
		drv_data->l_count = 0;
		drv_data->n_count = 0;
		if (drv_data->mode != 1 && drv_data->t_count > GMTR_DEBOUNCE)
			set_tablet_mode (drv_data, 1, angle);
	} else if (angle >= drv_data->laptop_thresh) {
		drv_data->n_count++;
		drv_data->t_count = 0;
		drv_data->l_count = 0;
	} else if (angle > drv_data->min_angle) {
		drv_data->l_count++;
		drv_data->t_count = 0;
		drv_data->n_count = 0;
		if (drv_data->mode != 0 && drv_data->l_count > GMTR_DEBOUNCE)
			set_tablet_mode (drv_data, 0, angle);
	}
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
		Vec3 a1_orient = a1;
		a1_orient.x = -a1_orient.x;
		update_orientation_debounce (sensor_device, drv_data, &a1_orient);
	}

	if (fabsf (a1.y) < GRAVITY_MIN && fabsf (a2.y) < GRAVITY_MIN)
		return G_SOURCE_CONTINUE;

	angle = compute_hinge_angle (drv_data, &a1, &a2);
	g_debug ("Hinge angle: %.1f  mode=%d", angle, drv_data->mode);
	update_tablet_mode (drv_data, angle);

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

	/* DSDT GMTR calibration matrices (defaults) */
	memcpy (drv_data->cal1, (gint8[]){ 1, 0, 0, 0, 1, 0, 0, 0, 1 }, 9);
	memcpy (drv_data->cal2, (gint8[]){ 1, 0, 0, 0, -1, 0, 0, 0, -1 }, 9);

	/* Try to load matrices and thresholds from ACPI DSDT */
	if (load_gmtr (drv_data))
		g_debug ("Dynamic calibration loaded from ACPI");
	else
		g_debug ("Using default calibration matrices");

	found = find_accels (drv_data->i2c_fds);
	if (found < 2) {
		g_debug ("Found %d accelerometers, trying unbind", found);
		for (gint i = 0; i < found; i++) {
			close (drv_data->i2c_fds[i]);
			drv_data->i2c_fds[i] = -1;
		}
		try_unbind ();
		found = find_accels (drv_data->i2c_fds);
	}

	if (found < 2) {
		g_debug ("Need 2 MXC6655 accelerometers, found %d", found);
		for (gint i = 0; i < found; i++)
			close (drv_data->i2c_fds[i]);
		g_free (drv_data);
		return NULL;
	}

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
send_initial_reading (SensorDevice *sensor_device)
{
	AccelReadings readings;

	build_synthetic_readings (MXC_ORIENT_RIGHT, &readings);
	sensor_device->callback_func (sensor_device,
				      (gpointer) &readings,
				      sensor_device->user_data);
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

	drv_data->timeout_id = g_timeout_add (GMTR_POLL_MS, poll_sensors, sensor_device);
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

	if (drv_data->want_polling == state)
		return;
	drv_data->want_polling = state;

	/* Send a first reading so a client's Claim() returns even if the lid is
	 * closed and the poll timer stays stopped. */
	if (state)
		send_initial_reading (sensor_device);

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
