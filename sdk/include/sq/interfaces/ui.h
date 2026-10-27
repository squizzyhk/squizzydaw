/* Squizzy Studio - module-facing UI services: theme:// resource resolution and remote-page
   descriptions for hardware surfaces (plan 2.5, M5). The in-process widget/renderer API stays in
   ui/include/sq/ui/*.hpp; this header is only what OTHER modules need.
   OWNER: orchestrator (frozen).
   IMPLEMENTS: ui/ (registers IThemeResolver and IRemotePageProvider via host->registerService, and
               an IUrlResolver for "theme://" with IObjectRegistry when scripting is present).
   CONSUMES: hardware-remote (remote pages), plugin hosts / services needing icons or colors,
             scripting-js, tools (skin compiler validation).
   THREADING: IThemeResolver is thread-safe (read-mostly; theme switches happen on the main thread
   and notify ISubject topic "theme.changed"). Remote page objects are immutable snapshots.

   theme:// URLS: theme://<themeName>/<path> or theme://current/<path>. Image lookups pick the
   best @2x/@3x variant for the requested scale; <path> never includes the @Nx suffix. */
#ifndef SQ_INTERFACES_UI_H
#define SQ_INTERFACES_UI_H

#include "sq/module.h"
#include "sq/interfaces/common.h"

/* sq_theme_resource.kind */
#define SQ_THEME_RES_IMAGE  1
#define SQ_THEME_RES_SKIN   2 /* skin XML */
#define SQ_THEME_RES_FONT   3
#define SQ_THEME_RES_COLORS 4 /* color scheme XML */
#define SQ_THEME_RES_OTHER  9

/* sq_remote_control.type */
#define SQ_REMOTE_KNOB    1
#define SQ_REMOTE_ENCODER 2 /* endless */
#define SQ_REMOTE_FADER   3
#define SQ_REMOTE_BUTTON  4
#define SQ_REMOTE_PAD     5
#define SQ_REMOTE_DISPLAY 6
#define SQ_REMOTE_LED     7

/* sq_remote_control.flags */
#define SQ_REMOTE_TOGGLE   0x01u /* button latches */
#define SQ_REMOTE_FEEDBACK 0x02u /* surface can display the value (motor fader, LED ring) */

typedef struct sq_theme_resource {
  uint32_t struct_size;
  int32_t kind;           /* SQ_THEME_RES_* */
  float scale;            /* variant chosen: 1, 2, 3 */
  int64_t size;           /* bytes */
  uint32_t width;         /* images: pixels of the chosen variant, else 0 */
  uint32_t height;
} sq_theme_resource;

typedef struct sq_remote_control {
  uint32_t struct_size;
  char id[64];            /* control id in the surface map (.surface.xml) */
  int32_t type;           /* SQ_REMOTE_* */
  char label[64];
  char target[256];       /* bound parameter: object:// URL (scripting.h IController + property) */
  char property[64];      /* property name on the target controller, e.g. "value", "mute" */
  uint32_t flags;         /* SQ_REMOTE_* */
} sq_remote_control;

#ifdef __cplusplus
namespace sq {

struct IThemeResolver : IUnknown {
  SQ_INTERFACE(IThemeResolver, IUnknown, "{EF89151C-E192-45DD-B79B-274BAB590479}")
  virtual sq_result SQ_CALL getActiveTheme(char* buf, uint32_t bufSize, uint32_t* needed) = 0;
  virtual sq_result SQ_CALL setActiveTheme(const char* name) = 0; /* main thread */
  virtual sq_result SQ_CALL resolve(const char* url, float scale, sq_theme_resource* out) = 0;
  virtual sq_result SQ_CALL openResource(const char* url, float scale, IByteStream** out) = 0;
  /* Named theme colors ("Track.Selected") as 0xRRGGBBAA and metrics ("Track.Height") in DIPs. */
  virtual sq_result SQ_CALL getColor(const char* name, uint32_t* rgba) = 0;
  virtual sq_result SQ_CALL getMetric(const char* name, double* value) = 0;
};

/* One page of a remote layout (a bank of controls). */
struct IRemotePage : IUnknown {
  SQ_INTERFACE(IRemotePage, IUnknown, "{09557537-F12C-4FAD-8D5C-98A3045B94D5}")
  virtual sq_result SQ_CALL getName(char* buf, uint32_t bufSize, uint32_t* needed) = 0;
  virtual uint32_t SQ_CALL getControlCount() = 0;
  virtual sq_result SQ_CALL getControl(uint32_t index, sq_remote_control* out) = 0;
};

/* Remote pages compiled from a skin document (same skin language as on-screen UIs). */
struct IRemotePageSet : IUnknown {
  SQ_INTERFACE(IRemotePageSet, IUnknown, "{9FC6D9FC-295C-4E03-A37F-92B4638B6F78}")
  virtual uint32_t SQ_CALL getPageCount() = 0;
  virtual sq_result SQ_CALL getPage(uint32_t index, IRemotePage** out) = 0;
};

struct IRemotePageProvider : IUnknown {
  SQ_INTERFACE(IRemotePageProvider, IUnknown, "{D3D37FB6-6AC9-4E02-80CD-112C57FC1886}")
  /* skinUrl: theme:// URL or file path of the remote skin; context: object:// URL the relative
     targets bind against (e.g. the focused track), may be NULL. */
  virtual sq_result SQ_CALL loadRemotePages(const char* skinUrl, const char* context, IRemotePageSet** out) = 0;
};

}  // namespace sq
#endif /* __cplusplus */

#endif /* SQ_INTERFACES_UI_H */
