#ifndef XENIA_UI_XDROID_UI_HOOKS_H_
#define XENIA_UI_XDROID_UI_HOOKS_H_

#include <atomic>

namespace xe {
namespace ui {

// XDroid360 LAT-1: called on the paint thread after a successful
// vkQueuePresentKHR. Set once by xdroid-core.
extern std::atomic<void (*)()> g_xdroid_on_present;

// XDroid360 MIRROR-1: while set, the paint skips the guest output and the
// screen stays black under the UI. Presents and guest output captures go on.
extern std::atomic<bool> g_xdroid_blank_local;

}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_XDROID_UI_HOOKS_H_
