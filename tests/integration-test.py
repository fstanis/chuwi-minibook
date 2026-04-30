#!/usr/bin/python3

# iio-sensor-proxy integration test suite
#
# Run in built tree to test local built binaries, or from anywhere else to test
# system installed binaries.
#
# Copyright: (C) 2011 Martin Pitt <martin.pitt@ubuntu.com>
# (C) 2021 Bastien Nocera <hadess@hadess.net>
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.

import os
import sys
import dbus
import dbusmock
import gi
import tempfile
import psutil
import subprocess
import unittest
import locale
import time

try:
    from gi.repository import GLib
    from gi.repository import Gio
except ImportError as e:
    sys.stderr.write('PyGobject not available for Python 3, or missing GI typelibs: %s\n' % str(e))
    sys.exit(1)

try:
    gi.require_version('UMockdev', '1.0')
    from gi.repository import UMockdev
except ImportError:
    sys.stderr.write('umockdev not available (https://github.com/martinpitt/umockdev)\n')
    sys.exit(1)


SP = 'net.hadess.SensorProxy'
SP_PATH = '/net/hadess/SensorProxy'
SP_COMPASS = 'net.hadess.SensorProxy.Compass'
SP_COMPASS_PATH = '/net/hadess/SensorProxy/Compass'

class Tests(dbusmock.DBusTestCase):

    @classmethod
    def setUpClass(cls):
        # run from local build tree if we are in one, otherwise use system instance
        builddir = os.getenv('top_builddir', '.')
        print(os.path.join(builddir, 'src', 'iio-sensor-proxy'))
        if os.access(os.path.join(builddir, 'src', 'iio-sensor-proxy'), os.X_OK):
            cls.daemon_path = os.path.join(builddir, 'src', 'iio-sensor-proxy')
            cls.monitor_sensor_path = os.path.join(builddir, 'src', 'monitor-sensor')
            print('Testing binaries from local build tree (%s)' % cls.daemon_path)
        elif os.environ.get('UNDER_JHBUILD', False):
            jhbuild_prefix = os.environ['JHBUILD_PREFIX']
            cls.daemon_path = os.path.join(jhbuild_prefix, 'libexec', 'iio-sensor-proxy')
            cls.monitor_sensor_path = os.path.join(jhbuild_prefix, 'bin', 'monitor-sensor')
            print('Testing binaries from JHBuild (%s)' % cls.daemon_path)
        else:
            cls.daemon_path = None
            with open('/usr/lib/systemd/system/iio-sensor-proxy.service') as f:
                for line in f:
                    if line.startswith('ExecStart='):
                        cls.daemon_path = line.split('=', 1)[1].strip()
                        break
            assert cls.daemon_path, 'could not determine daemon path from systemd .service file'
            cls.monitor_sensor_path = '/usr/bin/monitor-sensor'
            print('Testing installed system binary (%s)' % cls.daemon_path)

        # fail on CRITICALs on client and server side
        GLib.log_set_always_fatal(GLib.LogLevelFlags.LEVEL_WARNING |
                                  GLib.LogLevelFlags.LEVEL_ERROR |
                                  GLib.LogLevelFlags.LEVEL_CRITICAL)
        os.environ['G_DEBUG'] = 'fatal_warnings'

        # set up a fake system D-BUS
        cls.test_bus = Gio.TestDBus.new(Gio.TestDBusFlags.NONE)
        cls.test_bus.up()
        try:
            del os.environ['DBUS_SESSION_BUS_ADDRESS']
        except KeyError:
            pass
        os.environ['DBUS_SYSTEM_BUS_ADDRESS'] = cls.test_bus.get_bus_address()

        cls.dbus = Gio.bus_get_sync(Gio.BusType.SYSTEM, None)
        cls.dbus_con = cls.get_dbus(True)

        # Some test outputs require the daemon to run under the fr locale:
        # so check if that's available
        try:
            old_loc = locale.setlocale(locale.LC_ALL, 'fr_FR.UTF-8')
            locale.setlocale(locale.LC_ALL, old_loc)
	    # We need to make sure the decimal point is correct as on musl libc the above
	    # succeeds yet the tests just fail due to the output being in unexpected format
            cls.has_fr = locale.localeconv()["decimal_point"] == ","
        except:
            cls.has_fr = False

    @classmethod
    def tearDownClass(cls):
        cls.test_bus.down()
        dbusmock.DBusTestCase.tearDownClass()

    def setUp(self):
        '''Set up a local umockdev testbed.

        The testbed is initially empty.
        '''
        self.testbed = UMockdev.Testbed.new()
        self.polkitd, obj_polkit = self.spawn_server_template(
            'polkitd', {}, stdout=subprocess.PIPE)
        obj_polkit.SetAllowed(['net.hadess.SensorProxy.claim-sensor'])

        self.proxy = None
        self.log = None
        self.daemon = None

    def run(self, result=None):
        super(Tests, self).run(result)
        if result and len(result.errors) + len(result.failures) > 0 and self.log:
            with open(self.log.name) as f:
                sys.stderr.write('\n-------------- daemon log: ----------------\n')
                sys.stderr.write(f.read())
                sys.stderr.write('------------------------------\n')

    def tearDown(self):
        del self.testbed
        self.stop_daemon()

        if self.polkitd:
            try:
                self.polkitd.kill()
            except OSError:
                pass
            self.polkitd.wait()
        self.polkitd = None

    #
    # Daemon control and D-BUS I/O
    #

    def start_daemon(self, env = None, wrapper = None):
        '''Start daemon and create DBus proxy.

        When done, this sets self.proxy as the Gio.DBusProxy for power-profiles-daemon.
        '''
        if not env:
            env = os.environ.copy()
        env['G_DEBUG'] = 'fatal-criticals'
        env['G_MESSAGES_DEBUG'] = 'all'
        env['UMOCKDEV_DEBUG'] = 'all'
        # note: Python doesn't propagate the setenv from Testbed.new(), so we
        # have to do that ourselves
        env['UMOCKDEV_DIR'] = self.testbed.get_root_dir()
        self.log = tempfile.NamedTemporaryFile()
        timeout_multiplier = 1
        if wrapper:
            daemon_path = wrapper + [ self.daemon_path ]
        else:
            daemon_path = [ self.daemon_path ]
        if os.getenv('VALGRIND') != None:
            daemon_path = ['valgrind'] + daemon_path + ['-v']
            timeout_multiplier = 10
        else:
            daemon_path = daemon_path + ['-v']

        self.daemon = subprocess.Popen(daemon_path,
                                       env=env, stdout=self.log,
                                       stderr=subprocess.STDOUT)

        # wait until the daemon gets online
        timeout = 100 * timeout_multiplier
        while timeout > 0:
            time.sleep(0.1)
            timeout -= 1
            try:
                self.get_dbus_property('HasAccelerometer')
                break
            except GLib.GError:
                pass
        else:
            self.fail('daemon did not start in 10 seconds')

        self.proxy = Gio.DBusProxy.new_sync(
            self.dbus, Gio.DBusProxyFlags.DO_NOT_AUTO_START, None, SP,
            SP_PATH, SP, None)

        self.assertEqual(self.daemon.poll(), None, 'daemon crashed')

    def stop_daemon(self):
        '''Stop the daemon if it is running.'''

        if self.daemon:
            try:
                for child in psutil.Process(self.daemon.pid).children(recursive=True):
                    child.kill()
                self.daemon.kill()
            except OSError:
                pass
            self.daemon.wait()
        self.daemon = None
        self.proxy = None

    def get_dbus_property(self, name):
        '''Get property value from daemon D-Bus interface.'''

        proxy = Gio.DBusProxy.new_sync(
            self.dbus, Gio.DBusProxyFlags.DO_NOT_AUTO_START, None, SP,
            SP_PATH, 'org.freedesktop.DBus.Properties', None)
        return proxy.Get('(ss)', SP, name)

    def get_compass_dbus_property(self, name):
        '''Get property value from daemon compass D-Bus interface.'''

        proxy = Gio.DBusProxy.new_sync(
            self.dbus, Gio.DBusProxyFlags.DO_NOT_AUTO_START, None, SP,
            SP_COMPASS_PATH, 'org.freedesktop.DBus.Properties', None)
        return proxy.Get('(ss)', SP_COMPASS, name)

    def have_text_in_log(self, text):
        return self.count_text_in_log(text) > 0

    def count_text_in_log(self, text):
        with open(self.log.name) as f:
            return f.read().count(text)

    def read_sysfs_attr(self, device, attribute):
        with open(os.path.join(self.testbed.get_root_dir() + device, attribute), 'rb') as f:
            return f.read()
        return None

    def read_file(self, path):
        with open(path, 'rb') as f:
            return f.read()
        return None

    def assertEventually(self, condition, message=None, timeout=50):
        '''Assert that condition function eventually returns True.

        Timeout is in deciseconds, defaulting to 50 (5 seconds). message is
        printed on failure.
        '''
        while timeout >= 0:
            context = GLib.MainContext.default()
            while context.iteration(False):
                pass
            if condition():
                break
            timeout -= 1
            time.sleep(0.1)
        else:
            self.fail(message or 'timed out waiting for ' + str(condition))

    #
    # Actual test cases
    #

    def test_hwmon_light(self):
        '''hwmon light'''
        hwmon = self.testbed.add_device('platform', 'hwmon-als', None,
            ['light', '(128,128)'],
            ['IIO_SENSOR_PROXY_TYPE', 'hwmon-als']
        )
        self.start_daemon()
        self.assertEqual(self.get_dbus_property('HasAmbientLight'), True)
        self.assertEqual(self.get_dbus_property('HasAccelerometer'), False)

        # Default values
        self.assertEqual(self.get_dbus_property('LightLevelUnit'), 'lux')
        self.assertEqual(self.get_dbus_property('LightLevel'), 0)

        self.proxy.ClaimLight()
        self.assertEqual(int(self.get_dbus_property('LightLevel')), 50)
        self.assertEqual(self.get_dbus_property('LightLevelUnit'), 'vendor')

        self.testbed.set_attribute(hwmon, 'light', '(255,255)')
        # DEFAULT_POLL_TIME
        time.sleep(0.5)
        self.assertEqual(self.get_dbus_property('LightLevel'), 100)

        # process = subprocess.Popen(['gdbus', 'introspect', '--system', '--dest', 'net.hadess.SensorProxy', '--object-path', '/net/hadess/SensorProxy'])
        # print (self.get_dbus_property('Foo'))

        self.stop_daemon()

    def test_fake_light(self):
        '''fake light'''
        self.testbed.add_device('input', 'fake-light', None,
            [],
            ['NAME', '"Power Button"']
        )
        env = os.environ.copy()
        env['FAKE_LIGHT_SENSOR'] = '1'
        self.start_daemon(env)
        self.assertEqual(self.get_dbus_property('HasAmbientLight'), True)
        self.assertEqual(self.get_dbus_property('HasAccelerometer'), False)

        # Default values
        self.assertEqual(self.get_dbus_property('LightLevelUnit'), 'lux')
        self.assertEqual(self.get_dbus_property('LightLevel'), 0)

        self.proxy.ClaimLight()
        self.assertEqual(self.get_dbus_property('LightLevel'), 0.0)
        self.assertEqual(self.get_dbus_property('LightLevelUnit'), 'lux')

        self.assertEventually(lambda: int(self.get_dbus_property('LightLevel')) == 1.0)
        self.assertEqual(self.get_dbus_property('LightLevelUnit'), 'lux')

        # process = subprocess.Popen(['gdbus', 'introspect', '--system', '--dest', 'net.hadess.SensorProxy', '--object-path', '/net/hadess/SensorProxy'])
        # print (self.get_dbus_property('Foo'))

        self.stop_daemon()

    def test_fake_compass(self):
        '''fake compass'''
        self.testbed.add_device('input', 'fake-compass', None,
            [],
            ['NAME', '"Power Button"']
        )
        env = os.environ.copy()
        env['FAKE_COMPASS'] = '1'
        self.start_daemon(env)
        self.assertEqual(self.get_dbus_property('HasAmbientLight'), False)
        self.assertEqual(self.get_compass_dbus_property('HasCompass'), True)

        # Default values
        compass_proxy = Gio.DBusProxy.new_sync(
            self.dbus, Gio.DBusProxyFlags.DO_NOT_AUTO_START, None, SP,
            SP_COMPASS_PATH, SP_COMPASS, None)
        compass_proxy.ClaimCompass()

        self.assertEqual(int(self.get_compass_dbus_property('CompassHeading')), 10)
        self.assertEventually(lambda: int(self.get_compass_dbus_property('CompassHeading')) == 20)

        self.stop_daemon()

    def test_iio_accel_base_location(self):
        '''iio accel base location'''

        self.testbed.add_device('iio', 'iio-accel', None,
            ['in_accel_x_raw', '0',
             'in_accel_y_raw', '-25.6',
             'in_accel_z_raw', '0',
             'in_accel_scale', '10',
             'label', 'accel-base',
             'sampling_frequency', '5.2',
             'name', 'IIO Test Accelerometer'],
            ['NAME', '"IIO Accelerometer"',
             'IIO_SENSOR_PROXY_TYPE', 'iio-poll-accel']
        )
        self.testbed.add_device('input', 'fake-compass', None,
            [],
            ['NAME', '"Power Button"']
        )
        env = os.environ.copy()
        env['FAKE_COMPASS'] = '1'

        self.start_daemon(env)
        self.assertEqual(self.get_compass_dbus_property('HasCompass'), True)
        self.assertEqual(self.get_dbus_property('HasAccelerometer'), False)
        self.stop_daemon()

    def test_iio_poll_accel(self):
        '''iio poll accel'''
        accel = self.testbed.add_device('iio', 'iio-accel', None,
            ['in_accel_x_raw', '0',
             'in_accel_y_raw', '-25.6',
             'in_accel_z_raw', '0',
             'in_accel_scale', '10',
             'sampling_frequency', '5.2',
             'name', 'IIO Test Accelerometer'],
            ['NAME', '"IIO Accelerometer"',
             'IIO_SENSOR_PROXY_TYPE', 'iio-poll-accel']
        )
        self.start_daemon()
        self.assertEqual(self.get_dbus_property('HasAmbientLight'), False)
        self.assertEqual(self.get_dbus_property('HasAccelerometer'), True)

        # Default values
        self.assertEqual(self.get_dbus_property('AccelerometerOrientation'), 'undefined')
        self.assertEqual(self.get_dbus_property('AccelerometerTilt'), 'undefined')
        self.assertEqual(self.read_sysfs_attr(accel, 'sampling_frequency'), b'10')

        self.proxy.ClaimAccelerometer()
        self.assertEqual(self.get_dbus_property('AccelerometerOrientation'), 'normal')
        self.assertEqual(self.get_dbus_property('AccelerometerTilt'), 'vertical')

        self.testbed.set_attribute(accel, 'in_accel_x_raw', '-25.6')
        self.testbed.set_attribute(accel, 'in_accel_y_raw', '0')
        self.assertEventually(lambda: self.get_dbus_property('AccelerometerOrientation') == 'right-up')

        self.testbed.set_attribute(accel, 'in_accel_z_raw', '-25.6')
        self.assertEventually(lambda: self.get_dbus_property('AccelerometerTilt') == 'tilted-down')

        self.testbed.set_attribute(accel, 'in_accel_z_raw', '72.4')
        self.assertEventually(lambda: self.get_dbus_property('AccelerometerTilt') == 'face-up')

        self.stop_daemon()

    def test_iio_poll_light(self):
        '''iio poll light'''
        iio = self.testbed.add_device('iio', 'iio-als', None,
            ['integration_time', '1',
             'in_illuminance_input', '10',
             'in_illuminance_scale', '1.0'],
            ['NAME', '"IIO Light Sensor"',
             'IIO_SENSOR_PROXY_TYPE', 'iio-poll-als']
        )
        self.start_daemon()
        self.assertEqual(self.get_dbus_property('HasAmbientLight'), True)
        self.assertEqual(self.get_dbus_property('HasAccelerometer'), False)

        # Default values
        self.assertEqual(self.get_dbus_property('LightLevelUnit'), 'lux')
        self.assertEqual(self.get_dbus_property('LightLevel'), 0)

        self.proxy.ClaimLight()
        self.assertEqual(int(self.get_dbus_property('LightLevel')), 10)
        self.assertEqual(self.get_dbus_property('LightLevelUnit'), 'lux')

        self.testbed.set_attribute(iio, 'in_illuminance_input', '30')
        self.assertEventually(lambda: self.get_dbus_property('LightLevel') == 30)

        self.stop_daemon()

    def test_iio_poll_proximity(self):
        '''iio poll proximity'''
        prox = self.testbed.add_device('iio', 'iio-proximity', None,
            ['in_proximity_nearlevel', '128',
             'in_proximity_raw', '256',
             'name', 'IIO Test Proximity Sensor'],
            ['NAME', '"IIO Proximity Sensor"',
             'IIO_SENSOR_PROXY_TYPE', 'iio-poll-proximity']
        )
        self.start_daemon()
        self.assertEqual(self.get_dbus_property('HasAmbientLight'), False)
        self.assertEqual(self.get_dbus_property('HasAccelerometer'), False)
        self.assertEqual(self.get_dbus_property('HasProximity'), True)

        # Default values
        self.assertEqual(self.get_dbus_property('ProximityNear'), False)

        self.proxy.ClaimProximity()
        self.assertEqual(self.get_dbus_property('ProximityNear'), True)

        self.testbed.set_attribute(prox, 'in_proximity_raw', '0')
        self.assertEventually(lambda: self.get_dbus_property('ProximityNear') == False)

        self.testbed.set_attribute(prox, 'in_proximity_raw', '129')
        self.assertEventually(lambda: self.get_dbus_property('ProximityNear') == True)

        # Test margin
        self.testbed.set_attribute(prox, 'in_proximity_raw', '127')
        self.assertEventually(lambda: self.get_dbus_property('ProximityNear') == True)

        self.stop_daemon()

    def test_iio_poll_proximity0(self):
        '''iio poll proximity0'''
        prox = self.testbed.add_device('iio', 'iio-proximity', None,
            ['in_proximity_nearlevel', '128',
             'in_proximity0_raw', '256',
             'name', 'IIO Test Proximity Sensor'],
            ['NAME', '"IIO Proximity Sensor"',
             'IIO_SENSOR_PROXY_TYPE', 'iio-poll-proximity']
        )
        self.start_daemon()
        self.assertEqual(self.get_dbus_property('HasAmbientLight'), False)
        self.assertEqual(self.get_dbus_property('HasAccelerometer'), False)
        self.assertEqual(self.get_dbus_property('HasProximity'), True)

        # Default values
        self.assertEqual(self.get_dbus_property('ProximityNear'), False)

        self.proxy.ClaimProximity()
        self.assertEqual(self.get_dbus_property('ProximityNear'), True)

        self.testbed.set_attribute(prox, 'in_proximity0_raw', '0')
        self.assertEventually(lambda: self.get_dbus_property('ProximityNear') == False)

        self.testbed.set_attribute(prox, 'in_proximity0_raw', '129')
        self.assertEventually(lambda: self.get_dbus_property('ProximityNear') == True)

        # Test margin
        self.testbed.set_attribute(prox, 'in_proximity0_raw', '127')
        self.assertEventually(lambda: self.get_dbus_property('ProximityNear') == True)

        self.stop_daemon()

    def test_input_accel(self):
        '''input accelerometer'''
        top_srcdir = os.getenv('top_srcdir', '.')
        script = ['umockdev-run', '-d', top_srcdir + '/tests/input-accel-device',
                  '-i', '/dev/input/event4=%s/tests/input-accel-capture.ioctl' % (top_srcdir),
                  '--']

        self.start_daemon(wrapper=script)

        self.assertEqual(self.get_dbus_property('HasAccelerometer'), True)
        self.assertEqual(self.get_dbus_property('AccelerometerOrientation'), 'undefined')

        self.proxy.ClaimAccelerometer()
        self.assertEqual(self.get_dbus_property('AccelerometerOrientation'), 'normal')

        self.stop_daemon()

    def test_input_proximity(self):
        '''input proximity'''
        top_srcdir = os.getenv('top_srcdir', '.')
        script = ['umockdev-run', '-d', top_srcdir + '/tests/input-proximity-data/device',
                  '-i', '/dev/input/event4=%s/tests/input-proximity-data/ioctl' % (top_srcdir),
                  '--']

        self.start_daemon(wrapper=script)

        self.assertEqual(self.get_dbus_property('HasProximity'), True)
        self.assertEqual(self.get_dbus_property('ProximityNear'), False)

        self.stop_daemon()

    def test_iio_buffer_accel(self):
        '''iio buffer accel'''
        top_srcdir = os.getenv('top_srcdir', '.')
        mock_dev_data = self.testbed.get_root_dir() + '/iio-dev-data.bin'
        accel = self.testbed.add_device('iio', 'iio-buffer-accel0', None,
            ['name', 'IIO Test Accelerometer',
             'buffer/enable', '0',
             'trigger/current_trigger', '',
             'scan_elements/in_accel_x_en', '0',
             'scan_elements/in_accel_x_index', '0',
             'scan_elements/in_accel_x_type', 'le:s16/32>>0',
             'scan_elements/in_accel_y_en', '0',
             'scan_elements/in_accel_y_index', '1',
             'scan_elements/in_accel_y_type', 'le:s16/32>>0',
             'scan_elements/in_accel_z_en', '0',
             'scan_elements/in_accel_z_index', '2',
             'scan_elements/in_accel_z_type', 'le:s16/32>>0',
             'scan_elements/in_timestamp_en', '1',
             'scan_elements/in_timestamp_index', '3',
             'scan_elements/in_timestamp_type', 'le:s64/64>>0'],
            ['NAME', '"IIO Accelerometer"',
             'DEVNAME', '/dev/iio-buffer-accel-test',
             'IIO_SENSOR_PROXY_TYPE', 'iio-buffer-accel']
        )
        trigger = self.testbed.add_device('iio', 'trigger0', None,
            ['name', 'accel_3d-dev0'],
            []
        )
        data = self.read_file(top_srcdir + '/tests/iio-buffer-accel-data/orientation-normal.bin')
        with open(mock_dev_data,'wb') as mock_file:
            mock_file.write(data)
        self.start_daemon()
        self.assertEqual(self.get_dbus_property('HasAmbientLight'), False)
        self.assertEqual(self.get_dbus_property('HasAccelerometer'), True)

        # Default values
        self.assertEqual(self.get_dbus_property('AccelerometerOrientation'), 'undefined')
        self.assertEqual(self.read_sysfs_attr(accel, 'buffer/enable'), b'0')
        self.assertEqual(self.read_sysfs_attr(accel, 'scan_elements/in_accel_x_en'), b'1')
        self.assertEqual(self.read_sysfs_attr(accel, 'scan_elements/in_accel_y_en'), b'1')
        self.assertEqual(self.read_sysfs_attr(accel, 'scan_elements/in_accel_z_en'), b'1')

        with open(mock_dev_data,'wb') as mock_file:
            mock_file.write(data)
        self.proxy.ClaimAccelerometer()
        time.sleep(1)
        self.assertEqual(self.get_dbus_property('AccelerometerOrientation'), 'normal')
        os.remove(mock_dev_data)

        time.sleep(1)
        self.assertEqual(self.get_dbus_property('AccelerometerOrientation'), 'normal')

        data = self.read_file(top_srcdir + '/tests/iio-buffer-accel-data/orientation-left-up.bin')
        with open(mock_dev_data,'wb') as mock_file:
            mock_file.write(data)
        time.sleep(1)
        self.assertEqual(self.get_dbus_property('AccelerometerOrientation'), 'left-up')
        os.remove(mock_dev_data)

        data = self.read_file(top_srcdir + '/tests/iio-buffer-accel-data/orientation-normal.bin')
        with open(mock_dev_data,'wb') as mock_file:
            mock_file.write(data)
        time.sleep(1)
        self.assertEqual(self.get_dbus_property('AccelerometerOrientation'), 'normal')
        os.remove(mock_dev_data)

        self.stop_daemon()

    def test_driver_buffer_without_trigger(self):
        '''buffer driver picking without trigger'''
        top_srcdir = os.getenv('top_srcdir', '.')
        mock_dev_data = self.testbed.get_root_dir() + '/iio-dev-data.bin'
        accel = self.testbed.add_device('iio', 'iio-buffer-accel0', None,
            ['name', 'IIO Test Accelerometer',
             'buffer/enable', '0',
             'scan_elements/in_accel_x_en', '0',
             'scan_elements/in_accel_x_index', '0',
             'scan_elements/in_accel_x_type', 'le:s16/32>>0',
             'scan_elements/in_accel_y_en', '0',
             'scan_elements/in_accel_y_index', '1',
             'scan_elements/in_accel_y_type', 'le:s16/32>>0',
             'scan_elements/in_accel_z_en', '0',
             'scan_elements/in_accel_z_index', '2',
             'scan_elements/in_accel_z_type', 'le:s16/32>>0',
             'scan_elements/in_timestamp_en', '1',
             'scan_elements/in_timestamp_index', '3',
             'scan_elements/in_timestamp_type', 'le:s64/64>>0'],
            ['NAME', '"IIO Accelerometer"',
             'DEVNAME', '/dev/iio-buffer-accel-test-no-trigger',
             'IIO_SENSOR_PROXY_TYPE', 'iio-buffer-accel']
        )

        data = self.read_file(top_srcdir + '/tests/iio-buffer-accel-data/orientation-normal.bin')
        with open(mock_dev_data,'wb') as mock_file:
            mock_file.write(data)
        self.start_daemon()

        self.assertEqual(self.get_dbus_property('HasAmbientLight'), False)
        self.assertEqual(self.get_dbus_property('HasAccelerometer'), True)
        self.assertEqual(self.have_text_in_log('iio-buffer-accel'), True)

        self.stop_daemon()

    def test_iio_broken_buffer_accel(self):
        '''iio sensor reporting broken buffer support'''
        top_srcdir = os.getenv('top_srcdir', '.')
        mock_dev_data = self.testbed.get_root_dir() + '/iio-dev-data.bin'
        accel = self.testbed.add_device('iio', 'iio-buffer-accel0', None,
            ['name', 'IIO Test Accelerometer',
             'buffer/enable', '0',
             'trigger/current_trigger', '',
             'scan_elements/in_accel_x_en', '0',
             'scan_elements/in_accel_x_index', '0',
             'scan_elements/in_accel_x_type', 'le:s16/32>>0',
             'scan_elements/in_accel_y_en', '0',
             'scan_elements/in_accel_y_index', '1',
             'scan_elements/in_accel_y_type', 'le:s16/32>>0',
             'scan_elements/in_accel_z_en', '0',
             'scan_elements/in_accel_z_index', '2',
             'scan_elements/in_accel_z_type', 'le:s16/32>>0',
             'scan_elements/in_timestamp_en', '1',
             'scan_elements/in_timestamp_index', '3',
             'scan_elements/in_timestamp_type', 'le:s64/64>>0'],
            ['NAME', '"IIO Accelerometer"',
             'DEVNAME', '/dev/iio-buffer-accel-test',
             'IIO_SENSOR_PROXY_TYPE', 'iio-buffer-accel iio-poll-accel']
        )
        trigger = self.testbed.add_device('iio', 'trigger0', None,
            ['name', 'accel_3d-dev0'],
            []
        )
        self.start_daemon()
        self.assertEqual(self.have_text_in_log('Built channel array'), True)
        self.assertEqual(self.have_text_in_log('Disabled sensor'), True)
        self.assertEqual(self.have_text_in_log('Found IIO poll accelerometer'), True)
        self.assertEqual(self.get_dbus_property('HasAccelerometer'), True)

        self.stop_daemon()

    def test_unrequested_readings(self):
        '''unrequested property updates'''
        self.testbed.add_device('input', 'fake-light', None,
            [],
            ['NAME', '"Power Button"']
        )
        env = os.environ.copy()
        env['FAKE_LIGHT_SENSOR'] = '1'
        self.start_daemon(env)
        self.assertEqual(self.get_dbus_property('HasAmbientLight'), True)

        ctx = GLib.main_context_default()

        self.proxy.ClaimLight()
        self.assertEqual(self.get_dbus_property('LightLevel'), 0.0)
        self.proxy.ReleaseLight()

        self.assertEqual(self.get_dbus_property('LightLevel'), 0.0)

        self.proxy.ClaimLight()
        self.assertEqual(self.get_dbus_property('LightLevel'), 1.0)
        self.proxy.ReleaseLight()

        self.assertEqual(self.get_dbus_property('LightLevel'), 1.0)

        monitor = subprocess.Popen(self.monitor_sensor_path)
        self.assertEventually(lambda: int(self.get_dbus_property('LightLevel')) == 2.0)
        monitor.kill()

        # Catch up with main loop signals
        while ctx.pending():
            ctx.iteration(True)

        # We shouldn't have received light level properties updates
        res = self.proxy.get_cached_property('LightLevel')
        self.assertEqual(res.unpack(), 1.0)
        self.assertTrue(int(self.get_dbus_property('LightLevel') > 1.0))

        self.stop_daemon()

    def test_iio_scale_decimal_separator(self):
        '''scale decimal separator'''
        top_srcdir = os.getenv('top_srcdir', '.')
        mock_dev_data = self.testbed.get_root_dir() + '/iio-dev-data.bin'
        accel = self.testbed.add_device('iio', 'iio-buffer-accel0', None,
            ['name', 'IIO Test Accelerometer',
             'buffer/enable', '0',
             'trigger/current_trigger', '',
             'in_accel_scale', '0.000010\n',
             'in_accel_offset', '0.0\n',
             'in_accel_mount_matrix', '1, 0, 0; 0, 1, 0; 0, 0, 1\n',
             'scan_elements/in_accel_x_en', '0',
             'scan_elements/in_accel_x_index', '0',
             'scan_elements/in_accel_x_type', 'le:s16/32>>0',
             'scan_elements/in_accel_y_en', '0',
             'scan_elements/in_accel_y_index', '1',
             'scan_elements/in_accel_y_type', 'le:s16/32>>0',
             'scan_elements/in_accel_z_en', '0',
             'scan_elements/in_accel_z_index', '2',
             'scan_elements/in_accel_z_type', 'le:s16/32>>0',
             'scan_elements/in_timestamp_en', '1',
             'scan_elements/in_timestamp_index', '3',
             'scan_elements/in_timestamp_type', 'le:s64/64>>0'],
            ['NAME', '"IIO Accelerometer"',
             'DEVNAME', '/dev/iio-buffer-accel-test',
             'IIO_SENSOR_PROXY_TYPE', 'iio-buffer-accel']
        )
        trigger = self.testbed.add_device('iio', 'trigger0', None,
            ['name', 'accel_3d-dev0'],
            []
        )
        env = os.environ.copy()
        env['LC_NUMERIC'] = 'fr_FR.UTF-8'
        data = self.read_file(top_srcdir + '/tests/iio-buffer-accel-data/orientation-normal.bin')
        with open(mock_dev_data,'wb') as mock_file:
            mock_file.write(data)
        self.start_daemon(env=env)

        self.assertEqual(self.get_dbus_property('HasAccelerometer'), True)
        with open(mock_dev_data,'wb') as mock_file:
            mock_file.write(data)
        self.proxy.ClaimAccelerometer()
        self.assertEqual(self.have_text_in_log('Accel sent by driver'), True)
        self.assertEqual(self.have_text_in_log('scale: 0,000000,0,000000,0,000000'), False)
        if self.has_fr:
            self.assertEqual(self.have_text_in_log('scale: 0,000010,0,000010,0,000010'), True)
        self.stop_daemon()

    def test_iio_scale_decimal_separator_offset(self):
        '''scale decimal separator with specific offset'''
        top_srcdir = os.getenv('top_srcdir', '.')
        mock_dev_data = self.testbed.get_root_dir() + '/iio-dev-data.bin'
        accel = self.testbed.add_device('iio', 'iio-buffer-accel0', None,
            ['name', 'IIO Test Accelerometer',
             'buffer/enable', '0',
             'trigger/current_trigger', '',
             'in_accel_scale', '0.000010\n',
             'in_accel_x_offset', '0.0\n',
             'in_accel_mount_matrix', '1, 0, 0; 0, 1, 0; 0, 0, 1\n',
             'scan_elements/in_accel_x_en', '0',
             'scan_elements/in_accel_x_index', '0',
             'scan_elements/in_accel_x_type', 'le:s16/32>>0',
             'scan_elements/in_accel_y_en', '0',
             'scan_elements/in_accel_y_index', '1',
             'scan_elements/in_accel_y_type', 'le:s16/32>>0',
             'scan_elements/in_accel_z_en', '0',
             'scan_elements/in_accel_z_index', '2',
             'scan_elements/in_accel_z_type', 'le:s16/32>>0',
             'scan_elements/in_timestamp_en', '1',
             'scan_elements/in_timestamp_index', '3',
             'scan_elements/in_timestamp_type', 'le:s64/64>>0'],
            ['NAME', '"IIO Accelerometer"',
             'DEVNAME', '/dev/iio-buffer-accel-test',
             'IIO_SENSOR_PROXY_TYPE', 'iio-buffer-accel']
        )
        trigger = self.testbed.add_device('iio', 'trigger0', None,
            ['name', 'accel_3d-dev0'],
            []
        )
        env = os.environ.copy()
        env['LC_NUMERIC'] = 'fr_FR.UTF-8'
        data = self.read_file(top_srcdir + '/tests/iio-buffer-accel-data/orientation-normal.bin')
        with open(mock_dev_data,'wb') as mock_file:
            mock_file.write(data)
        self.start_daemon(env=env)

        self.assertEqual(self.get_dbus_property('HasAccelerometer'), True)

        self.stop_daemon()

    def test_sampling_freq_available_integer(self):
        'Check whether the sampling frequency gets corrected to >= 10Hz, integer numbers sorted low to high'

        accel = self.testbed.add_device('iio', 'iio-accel', None,
            ['in_accel_x_raw', '0',
             'in_accel_y_raw', '-256000000',
             'in_accel_z_raw', '0',
             'in_accel_scale', '0.000001',
             'sampling_frequency', '5.2',
             # Real world example taken from LSM303DA accelerometer
             'sampling_frequency_available', '3 6 12 25 50 100 200 400 800 1600',
             'name', 'IIO Test Accelerometer'],
            ['NAME', '"IIO Accelerometer"',
             'IIO_SENSOR_PROXY_TYPE', 'iio-poll-accel']
        )
        self.start_daemon()
        self.assertEqual(self.get_dbus_property('HasAccelerometer'), True)
        self.assertEqual(self.read_sysfs_attr(accel, 'sampling_frequency'), b'12')
        self.stop_daemon()

    def test_sampling_freq_available_double(self):
        'Check whether the sampling frequency gets corrected to >= 10Hz, double numbers sorted high to low'

        accel = self.testbed.add_device('iio', 'iio-accel', None,
            ['in_accel_x_raw', '0',
             'in_accel_y_raw', '-256000000',
             'in_accel_z_raw', '0',
             'in_accel_scale', '0.000001',
             'sampling_frequency', '5.2',
             # Real world example taken from drivers/iio/accel/mma8452.c
             'sampling_frequency_available', '800.000000 400.000000 200.000000 100.000000 50.000000 12.500000 6.250000 1.560000',
             'name', 'IIO Test Accelerometer'],
            ['NAME', '"IIO Accelerometer"',
             'IIO_SENSOR_PROXY_TYPE', 'iio-poll-accel']
        )
        self.start_daemon()
        self.assertEqual(self.get_dbus_property('HasAccelerometer'), True)
        self.assertEqual(self.read_sysfs_attr(accel, 'sampling_frequency'), b'12.5')
        self.stop_daemon()

    def test_iio_scale_decimal_separator2(self):
        '''scale decimal separator polling'''
        accel = self.testbed.add_device('iio', 'iio-accel', None,
            ['in_accel_x_raw', '0',
             'in_accel_y_raw', '-256000000',
             'in_accel_z_raw', '0',
             'in_accel_scale', '0.000001',
             'sampling_frequency', '5.2',
             'name', 'IIO Test Accelerometer'],
            ['NAME', '"IIO Accelerometer"',
             'IIO_SENSOR_PROXY_TYPE', 'iio-poll-accel']
        )
        env = os.environ.copy()
        env['LC_NUMERIC'] = 'fr_FR.UTF-8'
        self.start_daemon(env=env)

        self.proxy.ClaimAccelerometer()
        self.assertEqual(self.have_text_in_log('Accel read from IIO on'), True)
        self.assertEqual(self.have_text_in_log('scale 1,000000,1,000000,1,000000'), False)
        if self.has_fr:
            self.assertEqual(self.have_text_in_log('scale 0,000001,0,000001,0,000001'), True)

        self.assertEqual(self.get_dbus_property('AccelerometerOrientation'), 'normal')

        self.testbed.set_attribute(accel, 'in_accel_x_raw', '-256000000')
        self.testbed.set_attribute(accel, 'in_accel_y_raw', '0')
        self.assertEventually(lambda: self.get_dbus_property('AccelerometerOrientation') == 'right-up')

        self.stop_daemon()

    #
    # Helper methods
    #

    @classmethod
    def _props_to_str(cls, properties):
        '''Convert a properties dictionary to uevent text representation.'''

        prop_str = ''
        if properties:
            for k, v in properties.items():
                prop_str += '%s=%s\n' % (k, v)
        return prop_str

if __name__ == '__main__':
    # run ourselves under umockdev
    if 'umockdev' not in os.environ.get('LD_PRELOAD', ''):
        os.execvp('umockdev-wrapper', ['umockdev-wrapper'] + sys.argv)

    unittest.main()
