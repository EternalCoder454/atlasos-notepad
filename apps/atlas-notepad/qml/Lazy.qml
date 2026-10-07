// A menu, dialog or popover made the first time it is wanted (`get()`), then
// kept: most sessions never open most of them, and each costs memory and
// start-up time. `host` is the item it would have been declared in, so it
// sits and opens exactly as it did there.
import QtQuick

QtObject {
    id: lazy

    required property Component component
    required property Item host
    // The object, once made; null before.
    property var object: null
    // The object is made and showing (a popup's `visible`).
    readonly property bool visible: object !== null && object.visible

    function get(): var {
        if (lazy.object === null) {
            lazy.object = lazy.component.createObject(lazy.host);
        }
        return lazy.object;
    }
}
