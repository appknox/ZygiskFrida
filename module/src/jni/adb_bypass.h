#pragma once

// Installs a Dobby ioctl hook that corrupts the setting name in the outgoing
// binder parcel for Settings.Global.getInt(adb_enabled /
// development_settings_enabled), so the query returns 0 before onCreate.
void install_adb_bypass();
