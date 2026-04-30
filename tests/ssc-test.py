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

    @unittest.skip('Proximity SSC sensor is currently disabled via udev by default.')
    def test_ssc_proximity(self):
        '''SSC proximity'''
        prox = self.testbed.add_device('misc', 'ssc-proximity', None,
            ['name', 'SSC Test Proximity Sensor'],
            ['NAME', '"SSC Proximity Sensor"',
             'DEVNAME', '/dev/fastrpc-sdsp',
             'IIO_SENSOR_PROXY_TYPE', 'ssc-proximity']
        )
        self.start_daemon()
        self.assertEqual(self.get_dbus_property('HasAmbientLight'), False)
        self.assertEqual(self.get_dbus_property('HasAccelerometer'), False)
        self.assertEqual(self.get_dbus_property('HasProximity'), True)
        self.assertEqual(self.get_compass_dbus_property('HasCompass'), False)

        # Default values
        self.assertEqual(self.get_dbus_property('ProximityNear'), False)

        self.proxy.ClaimProximity()
        self.assertEqual(self.get_dbus_property('ProximityNear'), True)
        self.assertEventually(lambda: self.get_dbus_property('ProximityNear') == False)
        self.assertEventually(lambda: self.get_dbus_property('ProximityNear') == True)

        self.stop_daemon()

    @unittest.skip('Accelerometer SSC sensor is currently disabled via udev by default.')
    def test_ssc_accel(self):
        '''SSC accelerometer'''
        prox = self.testbed.add_device('misc', 'ssc-accel', None,
            ['name', 'SSC Test Accelerometer Sensor'],
            ['NAME', '"SSC Accelerometer Sensor"',
             'DEVNAME', '/dev/fastrpc-sdsp',
             'IIO_SENSOR_PROXY_TYPE', 'ssc-accel']
        )
        self.start_daemon()
        self.assertEqual(self.get_dbus_property('HasAmbientLight'), False)
        self.assertEqual(self.get_dbus_property('HasAccelerometer'), True)
        self.assertEqual(self.get_dbus_property('HasProximity'), False)
        self.assertEqual(self.get_compass_dbus_property('HasCompass'), False)

        self.assertEqual(self.get_dbus_property('AccelerometerOrientation'), 'undefined')

        self.proxy.ClaimAccelerometer()
        self.assertEqual(self.get_dbus_property('AccelerometerOrientation'), 'left-up')

        self.stop_daemon()

    def test_ssc_light(self):
        '''SSC light'''
        prox = self.testbed.add_device('misc', 'ssc-light', None,
            ['name', 'SSC Test Light Sensor'],
            ['NAME', '"SSC Light Sensor"',
             'DEVNAME', '/dev/fastrpc-sdsp',
             'IIO_SENSOR_PROXY_TYPE', 'ssc-light']
        )
        self.start_daemon()
        self.assertEqual(self.get_dbus_property('HasAmbientLight'), True)
        self.assertEqual(self.get_dbus_property('HasAccelerometer'), False)
        self.assertEqual(self.get_dbus_property('HasProximity'), False)
        self.assertEqual(self.get_compass_dbus_property('HasCompass'), False)

        # Default values
        self.assertEqual(self.get_dbus_property('LightLevelUnit'), 'lux')
        self.assertEqual(self.get_dbus_property('LightLevel'), 0)

        self.proxy.ClaimLight()
        self.assertEventually(lambda: self.get_dbus_property('LightLevel') == 7)
        self.assertEqual(self.get_dbus_property('LightLevelUnit'), 'lux')

        self.stop_daemon()

    def test_ssc_compass(self):
        '''SSC compass'''
        prox = self.testbed.add_device('misc', 'ssc-compass', None,
            ['name', 'SSC Test Compass Sensor'],
            ['NAME', '"SSC Compass Sensor"',
             'DEVNAME', '/dev/fastrpc-sdsp',
             'IIO_SENSOR_PROXY_TYPE', 'ssc-compass']
        )
        self.start_daemon()
        self.assertEqual(self.get_dbus_property('HasAmbientLight'), False)
        self.assertEqual(self.get_dbus_property('HasAccelerometer'), False)
        self.assertEqual(self.get_dbus_property('HasProximity'), False)
        self.assertEqual(self.get_compass_dbus_property('HasCompass'), True)
        self.assertEqual(int(self.get_compass_dbus_property('CompassHeading')), 0)

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
