// What Notepad was called before it was Telamon Notepad (atlas-notepad,
// net.eterneon.atlas.notepad), and the once-only moves from those names.
// Kept for one release: the image upgrades Notepad in place, with the old
// one possibly still running.
#pragma once

#include <QString>
#include <QStringList>

namespace Legacy
{
// ~/.config/atlas-notepadrc (QStandardPaths::GenericConfigLocation)
QString configFile();
// $XDG_STATE_HOME/atlas-notepad: session/ and session.lock
QString stateDir();
// $XDG_DATA_HOME/atlas-notepad: where the session lived before 0.1
QString dataDir();
// The old app ID: its desktop file, its D-Bus name.
const QString &appId();

// rename(2) that never replaces what is at `to` (an existing file or a
// folder, empty or not): true when `from` is now `to`, in one step.
bool moveNoReplace(const QString &from, const QString &to);

// True when another process holds the old Notepad's session lock, i.e. an
// atlas-notepad is running and writing the old session.
bool sessionHeldByOldNotepad();

// An atlas-notepad is running (it has net.eterneon.atlas.notepad on the
// session bus): hands it this launch, as a second launch of it would be
// handed over, and returns true. False: none running, or it didn't answer.
bool forwardLaunch(const QStringList &arguments);
}
