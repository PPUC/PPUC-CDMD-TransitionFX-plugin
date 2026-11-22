// license:GPLv3+
//
// Tests of the TransitionFX: the effects on their own (every transition, every option, on
// several frame sizes), and the plugin against a fake editor that implements the part of the
// editor API the plugin uses, with edit batches that can be taken back. The window is scripted
// the way the author uses it: menu entry, then fields changed one by one, then Apply.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "ColorizingDMDPlugin.h"
#include "Preview.h"
#include "Language.h"
#include "Transitions.h"
#include "Version.h"
#include "plugins/MsgPlugin.h"

MSGPI_EXPORT void MSGPIAPI TransitionFXPluginLoad(const uint32_t, const MsgPluginAPI*);
MSGPI_EXPORT void MSGPIAPI TransitionFXPluginUnload();

namespace {

int g_failures = 0;
#define CHECK(cond)                                                                      \
   do                                                                                    \
   {                                                                                     \
      if (!(cond))                                                                       \
      {                                                                                  \
         std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
         ++g_failures;                                                                   \
      }                                                                                  \
   } while (0)

// ---- The fake project -----------------------------------------------------------------------

uint32_t g_w = 128, g_h = 32; // the original frame and the SD plane
constexpr uint32_t kColors = 4;

struct Frame
{
   std::vector<uint8_t> original;
   std::vector<uint16_t> sd;
   std::vector<uint8_t> zones;
   std::vector<uint16_t> dynamicColors; // 32 sets x kColors, empty: none
   int rotations = 0;
   uint32_t background = CDMDPI_NONE;
   uint32_t comparisonMask = CDMDPI_NONE;
   uint32_t shapeMode = 0;
   uint32_t durationMs = 50;
   uint32_t startsScene = CDMDPI_NONE; // the scene the frame starts
   uint32_t inScene = CDMDPI_NONE;     // the scene the frame belongs to
};

struct Scene
{
   uint32_t id = 0;
   std::string name;
   uint32_t durationMs = 0;
   uint32_t repeat = 0;
   uint32_t finish = 0;
   uint32_t flags = 0;
   uint32_t frames = 0;
};

std::vector<Frame> g_frames;
std::vector<Frame> g_snapshot;
std::vector<Scene> g_scenes;
std::vector<Scene> g_sceneSnapshot;
bool g_inBatch = false;
uint64_t g_revision = 1;
uint32_t g_current = 0;
int g_addFrameLimit = -1;
int g_addFrameCalls = 0;
bool g_refuseMove = false;
bool g_refuseScene = false;

std::map<std::string, std::string> g_settings;
std::vector<std::string> g_messages;
std::vector<std::string> g_status;
int g_endEditApplied = 0;
std::vector<uint32_t> g_shown;
int g_showCalls = 0; // the plugin leaves the current frame and the selection alone, so Undo returns to them
cdmdpi_action_callback g_menuCallback = nullptr;

// The live form the plugin opened last.
struct Form
{
   std::string title, accept;
   struct Image
   {
      uint32_t width, height, scale;
      std::string caption;
      std::vector<uint16_t> pixels;
   };
   struct Field
   {
      std::string key, label, value, choices, help;
      uint32_t kind;
      bool visible = true;
      bool enabled = true;
      std::vector<Image> images;
   };
   std::vector<Field> fields;
   cdmdpi_form_changed_callback changed = nullptr;
   cdmdpi_form_closed_callback closed = nullptr;
   void* userData = nullptr;
   bool open = false;
};
Form g_form;
int g_formsOpened = 0;
int g_fieldUpdates = 0;

Frame makeSourceFrame(bool withZones)
{
   Frame f;
   f.original.resize(static_cast<size_t>(g_w) * g_h);
   f.sd.resize(f.original.size());
   for (uint32_t y = 0; y < g_h; ++y)
      for (uint32_t x = 0; x < g_w; ++x)
      {
         const size_t i = static_cast<size_t>(y) * g_w + x;
         f.original[i] = static_cast<uint8_t>(1 + (x + 2 * y) % 3);
         // RGB565 colors that differ from pixel to pixel but never 0 (black).
         f.sd[i] = static_cast<uint16_t>(0x0841 * (1 + f.original[i]) + (i % 5) * 0x0020);
      }
   if (withZones)
   {
      f.zones.resize(f.original.size());
      for (size_t i = 0; i < f.zones.size(); ++i)
         f.zones[i] = static_cast<uint8_t>(i % 4);
      f.dynamicColors.resize(32 * kColors);
      for (size_t i = 0; i < f.dynamicColors.size(); ++i)
         f.dynamicColors[i] = static_cast<uint16_t>(0x2104 * (1 + i % kColors));
      f.rotations = 2;
   }
   return f;
}

// ---- The fake editor API --------------------------------------------------------------------

int MSGPIAPI fGetProjectInfo(ColorizingDMDProjectInfo* info)
{
   std::memset(info, 0, sizeof(*info));
   info->hasProject = 1;
   info->frameCount = static_cast<uint32_t>(g_frames.size());
   info->colorCount = kColors;
   info->originalWidth = g_w;
   info->originalHeight = g_h;
   info->sdWidth = g_w;
   info->sdHeight = g_h;
   info->revision = g_revision;
   return 1;
}

int MSGPIAPI fGetFrameInfo(uint32_t frame, ColorizingDMDFrameInfo* info)
{
   if (frame >= g_frames.size())
      return 0;
   std::memset(info, 0, sizeof(*info));
   info->durationMs = g_frames[frame].durationMs;
   info->comparisonMask = g_frames[frame].comparisonMask;
   info->shapeMode = g_frames[frame].shapeMode;
   info->background = g_frames[frame].background;
   info->hasDynamicPixels = g_frames[frame].zones.empty() ? 0 : 1;
   info->hasHd = 0;
   info->triggerId = g_frames[frame].startsScene;
   info->section = CDMDPI_NONE;
   return 1;
}

template <class T>
uint32_t give(const std::vector<T>& data, T* out, uint32_t capacity, uint32_t* width, uint32_t* height)
{
   *width = g_w;
   *height = g_h;
   if (data.empty())
      return 0;
   if (capacity >= data.size())
      std::memcpy(out, data.data(), data.size() * sizeof(T));
   return static_cast<uint32_t>(data.size());
}

uint32_t MSGPIAPI fGetOriginalFrame(uint32_t frame, uint8_t* px, uint32_t cap, uint32_t* w, uint32_t* h)
{
   return give(g_frames.at(frame).original, px, cap, w, h);
}
uint32_t MSGPIAPI fGetColorizedFrame(uint32_t frame, int hd, uint16_t* px, uint32_t cap, uint32_t* w, uint32_t* h)
{
   if (hd)
      return 0;
   return give(g_frames.at(frame).sd, px, cap, w, h);
}
uint32_t MSGPIAPI fGetDynamicMask(uint32_t frame, uint8_t* px, uint32_t cap, uint32_t* w, uint32_t* h)
{
   return give(g_frames.at(frame).zones, px, cap, w, h);
}
uint32_t MSGPIAPI fGetDynamicColors(uint32_t frame, uint16_t* colors, uint32_t cap)
{
   const auto& c = g_frames.at(frame).dynamicColors;
   if (c.empty())
      return 0;
   if (cap >= c.size())
      std::memcpy(colors, c.data(), c.size() * sizeof(uint16_t));
   return static_cast<uint32_t>(c.size());
}

uint32_t MSGPIAPI fGetCurrentFrame() { return g_current; }

// The selected frames, ascending; just the current one when none is set.
std::vector<uint32_t> g_selected;
uint32_t MSGPIAPI fGetSelectedFrames(uint32_t* frames, uint32_t cap)
{
   std::vector<uint32_t> list = g_selected;
   if (list.empty() && g_current != CDMDPI_NONE)
      list.push_back(g_current);
   for (uint32_t i = 0; i < list.size() && i < cap; ++i)
      frames[i] = list[i];
   return static_cast<uint32_t>(list.size());
}

int MSGPIAPI fShowFrames(uint32_t, const uint32_t* frames, uint32_t count)
{
   ++g_showCalls;
   g_shown.assign(frames, frames + count);
   return 1;
}

void MSGPIAPI fAddMenuEntry(uint32_t, const char*, cdmdpi_action_callback cb, void*) { g_menuCallback = cb; }
void MSGPIAPI fRemoveMenuEntries(uint32_t) { g_menuCallback = nullptr; }
void MSGPIAPI fShowMessage(uint32_t, const char*, const char* text) { g_messages.push_back(text); }
void MSGPIAPI fShowStatus(uint32_t, const char* text) { g_status.push_back(text); }

uint32_t copyText(const std::string& text, char* value, uint32_t capacity)
{
   const uint32_t size = static_cast<uint32_t>(text.size() + 1);
   if (capacity >= size)
      std::memcpy(value, text.c_str(), size);
   return size;
}

uint32_t MSGPIAPI fGetSetting(uint32_t, const char* key, char* value, uint32_t capacity)
{
   auto it = g_settings.find(key);
   if (it == g_settings.end())
      return 0;
   return copyText(it->second, value, capacity);
}
void MSGPIAPI fSetSetting(uint32_t, const char* key, const char* value) { g_settings[key] = value; }

int MSGPIAPI fBeginEdit(uint32_t, const char*)
{
   if (g_inBatch)
      return 0;
   g_inBatch = true;
   g_snapshot = g_frames;
   g_sceneSnapshot = g_scenes;
   g_addFrameCalls = 0;
   return 1;
}
int MSGPIAPI fEndEdit(uint32_t, int apply)
{
   if (!g_inBatch)
      return 0;
   g_inBatch = false;
   if (apply)
   {
      ++g_endEditApplied;
      ++g_revision;
   }
   else
   {
      g_frames = g_snapshot;
      g_scenes = g_sceneSnapshot;
   }
   return 1;
}

int MSGPIAPI fAddFrame(uint32_t, const uint8_t* original, uint32_t count, uint32_t like, uint32_t* frame)
{
   if (!g_inBatch || like >= g_frames.size() || count != g_w * g_h)
      return 0;
   if (g_addFrameLimit >= 0 && g_addFrameCalls >= g_addFrameLimit)
      return 0;
   ++g_addFrameCalls;
   Frame f = g_frames[like];
   f.original.assign(original, original + count);
   f.startsScene = CDMDPI_NONE; // AddFrame leaves the trigger out
   g_frames.push_back(f);
   if (frame)
      *frame = static_cast<uint32_t>(g_frames.size() - 1);
   return 1;
}

int MSGPIAPI fSetFramePicture(uint32_t, uint32_t frame, int hd, const uint16_t* pixels, const uint8_t* write, uint32_t count)
{
   if (!g_inBatch || frame >= g_frames.size() || hd)
      return 0;
   auto& target = g_frames[frame].sd;
   if (count != target.size())
      return 0;
   for (uint32_t i = 0; i < count; ++i)
      if (!write || write[i])
         target[i] = pixels[i];
   return 1;
}

int MSGPIAPI fSetFrameComparison(uint32_t, uint32_t frame, uint32_t mask, uint32_t shapeMode)
{
   if (!g_inBatch || frame >= g_frames.size())
      return 0;
   g_frames[frame].comparisonMask = mask;
   g_frames[frame].shapeMode = shapeMode;
   return 1;
}

int MSGPIAPI fSetDynamicZones(uint32_t, uint32_t frame, const uint8_t* sets, uint32_t count)
{
   if (!g_inBatch || frame >= g_frames.size() || count != g_frames[frame].zones.size())
      return 0;
   g_frames[frame].zones.assign(sets, sets + count);
   return 1;
}

int MSGPIAPI fSetDynamicColors(uint32_t, uint32_t frame, uint32_t set, const uint16_t* colors, uint32_t count)
{
   if (!g_inBatch || frame >= g_frames.size() || g_frames[frame].dynamicColors.empty() || count != kColors || set >= 32)
      return 0;
   std::memcpy(&g_frames[frame].dynamicColors[static_cast<size_t>(set) * kColors], colors, count * sizeof(uint16_t));
   return 1;
}

int MSGPIAPI fClearColorRotations(uint32_t, uint32_t frame)
{
   if (!g_inBatch || frame >= g_frames.size())
      return 0;
   g_frames[frame].rotations = 0;
   return 1;
}

int MSGPIAPI fMoveFrames(uint32_t, const uint32_t* frames, uint32_t count, uint32_t before)
{
   if (!g_inBatch || g_refuseMove || before > g_frames.size() || count == 0)
      return 0;
   std::vector<bool> moving(g_frames.size(), false);
   for (uint32_t i = 0; i < count; ++i)
   {
      if (frames[i] >= g_frames.size())
         return 0;
      moving[frames[i]] = true;
   }
   std::vector<Frame> rest, block, result;
   size_t insertAt = 0; // among the frames that stay: those numbered below `before`
   for (size_t i = 0; i < g_frames.size(); ++i)
   {
      if (moving[i])
         block.push_back(g_frames[i]);
      else
      {
         if (i < before)
            ++insertAt;
         rest.push_back(g_frames[i]);
      }
   }
   result.assign(rest.begin(), rest.begin() + static_cast<long>(insertAt));
   result.insert(result.end(), block.begin(), block.end());
   result.insert(result.end(), rest.begin() + static_cast<long>(insertAt), rest.end());
   g_frames = result;
   return 1;
}

int MSGPIAPI fDeleteFrames(uint32_t, const uint32_t* frames, uint32_t count)
{
   if (!g_inBatch || count == 0 || count >= g_frames.size())
      return 0;
   std::vector<bool> deleting(g_frames.size(), false);
   for (uint32_t i = 0; i < count; ++i)
   {
      if (frames[i] >= g_frames.size())
         return 0;
      deleting[frames[i]] = true;
   }
   std::vector<Frame> kept;
   for (size_t i = 0; i < g_frames.size(); ++i)
      if (!deleting[i])
         kept.push_back(g_frames[i]);
   g_frames = kept;
   return 1;
}

int MSGPIAPI fCreateScene(uint32_t, const ColorizingDMDScene* scene, const uint32_t* frames, uint32_t count,
                          uint32_t* sceneId, uint32_t* firstFrame)
{
   if (!g_inBatch || g_refuseScene || !scene || count == 0 || g_w != 128 || g_h != 32 ||
       scene->durationMs > 65535 || scene->groups > 1)
      return 0;
   for (uint32_t i = 0; i < count; ++i)
      if (frames[i] >= g_frames.size() || g_frames[frames[i]].inScene != CDMDPI_NONE)
         return 0;
   uint32_t id = scene->sceneId;
   if (id == CDMDPI_NONE)
   {
      id = 50000;
      for (const Scene& s : g_scenes)
         id = std::max(id, s.id + 1);
   }
   Scene made;
   made.id = id;
   made.name = scene->name ? scene->name : "";
   made.durationMs = scene->durationMs ? scene->durationMs : 33;
   made.repeat = scene->repeat;
   made.finish = scene->finish;
   made.flags = scene->flags;
   made.frames = count;
   g_scenes.push_back(made);
   const uint32_t first = static_cast<uint32_t>(g_frames.size());
   for (uint32_t i = 0; i < count; ++i)
   {
      Frame f = g_frames[frames[i]];
      f.zones.clear(); // a frame of a scene has no dynamic zones
      f.startsScene = CDMDPI_NONE;
      f.inScene = id;
      g_frames.push_back(f);
   }
   if (sceneId)
      *sceneId = id;
   if (firstFrame)
      *firstFrame = first;
   return 1;
}

int MSGPIAPI fSetFrameScene(uint32_t, uint32_t frame, uint32_t scene)
{
   if (!g_inBatch || frame >= g_frames.size() || g_frames[frame].inScene != CDMDPI_NONE)
      return 0;
   if (scene != CDMDPI_NONE &&
       std::none_of(g_scenes.begin(), g_scenes.end(), [&](const Scene& s) { return s.id == scene; }))
      return 0;
   g_frames[frame].startsScene = scene;
   return 1;
}

// ---- The fake live form ---------------------------------------------------------------------

Form::Field* fieldOf(const char* key)
{
   for (auto& f : g_form.fields)
      if (f.key == key)
         return &f;
   return nullptr;
}

// The values a CHOICE offers ("value=label" lines or plain values).
std::vector<std::string> choiceValues(const std::string& choices)
{
   std::vector<std::string> out;
   size_t start = 0;
   while (start <= choices.size())
   {
      size_t end = choices.find('\n', start);
      if (end == std::string::npos)
         end = choices.size();
      std::string line = choices.substr(start, end - start);
      const size_t eq = line.find('=');
      if (eq != std::string::npos)
         line = line.substr(0, eq);
      if (!line.empty())
         out.push_back(line);
      start = end + 1;
   }
   return out;
}
std::vector<std::string> choiceValues(const Form::Field& f) { return choiceValues(f.choices); }

// A choice, as a drop-down list or as radio buttons (version 10).
bool isChoice(uint32_t kind)
{
   return kind == CDMDPI_FIELD_CHOICE || kind == CDMDPI_FIELD_RADIO;
}

bool offers(const std::string& choices, const std::string& value)
{
   const auto values = choiceValues(choices);
   return std::find(values.begin(), values.end(), value) != values.end();
}

std::vector<Form::Image> copyImages(const ColorizingDMDFormImage* images, uint32_t count)
{
   std::vector<Form::Image> out;
   for (uint32_t i = 0; i < count; ++i)
   {
      const ColorizingDMDFormImage& image = images[i];
      out.push_back({image.width, image.height, image.scale, image.caption ? image.caption : "",
                     std::vector<uint16_t>(image.pixels, image.pixels + static_cast<size_t>(image.width) * image.height)});
   }
   return out;
}

int MSGPIAPI fShowLiveForm(uint32_t, const char* title, const char* accept, const char* const*, uint32_t extraCount,
                           const ColorizingDMDFormField* fields, uint32_t count, cdmdpi_form_changed_callback changed,
                           cdmdpi_form_closed_callback closed, void* userData)
{
   if (g_form.open || !fields || count == 0 || !closed || extraCount > CDMDPI_FORM_MAX_BUTTONS)
      return 0;
   ++g_formsOpened;
   g_form = Form{};
   g_form.title = title;
   g_form.accept = accept ? accept : "";
   g_form.changed = changed;
   g_form.closed = closed;
   g_form.userData = userData;
   g_form.open = true;
   for (uint32_t i = 0; i < count; ++i)
   {
      const ColorizingDMDFormField& in = fields[i];
      Form::Field f;
      f.key = in.key ? in.key : "";
      f.label = in.label ? in.label : "";
      f.value = in.defaultValue ? in.defaultValue : "";
      f.choices = in.choices ? in.choices : "";
      f.help = in.help ? in.help : "";
      f.kind = in.kind;
      f.visible = !(in.flags & CDMDPI_FORM_HIDDEN);
      f.enabled = !(in.flags & CDMDPI_FORM_DISABLED);
      if (in.kind == CDMDPI_FIELD_IMAGE)
         f.images = copyImages(in.images, in.imageCount);
      // As the editor: a choice opens on its first entry when the value is not in the list.
      if (isChoice(in.kind) && !offers(f.choices, f.value))
         f.value = choiceValues(f.choices).empty() ? std::string() : choiceValues(f.choices).front();
      g_form.fields.push_back(f);
   }
   return 1;
}

uint32_t MSGPIAPI fGetFormValue(uint32_t, const char* key, char* value, uint32_t capacity)
{
   const Form::Field* f = g_form.open || g_form.closed ? fieldOf(key) : nullptr;
   if (!f || f->kind == CDMDPI_FIELD_NOTE || f->kind == CDMDPI_FIELD_IMAGE)
      return 0;
   return copyText(f->value, value, capacity);
}

int MSGPIAPI fSetFormField(uint32_t, const char* key, const ColorizingDMDFormFieldUpdate* update)
{
   Form::Field* f = g_form.open ? fieldOf(key) : nullptr;
   if (!f || !update)
      return 0;
   const uint32_t c = update->changes;
   if ((c & CDMDPI_FORM_SET_VALUE) && (f->kind == CDMDPI_FIELD_NOTE || f->kind == CDMDPI_FIELD_IMAGE))
      return 0;
   if ((c & CDMDPI_FORM_SET_CHOICES) && (!isChoice(f->kind) || !update->choices || !*update->choices))
      return 0;
   if ((c & CDMDPI_FORM_SET_IMAGES) && f->kind != CDMDPI_FIELD_IMAGE)
      return 0;
   const std::string choices = (c & CDMDPI_FORM_SET_CHOICES) ? update->choices : f->choices;
   if ((c & CDMDPI_FORM_SET_VALUE) && isChoice(f->kind) && !offers(choices, update->value ? update->value : ""))
      return 0;
   ++g_fieldUpdates;
   if (c & CDMDPI_FORM_SET_ENABLED)
      f->enabled = update->enabled != 0;
   if (c & CDMDPI_FORM_SET_VISIBLE)
      f->visible = update->visible != 0;
   if (c & CDMDPI_FORM_SET_LABEL)
      f->label = update->label ? update->label : "";
   if (c & CDMDPI_FORM_SET_HELP)
      f->help = update->help ? update->help : "";
   if (c & CDMDPI_FORM_SET_IMAGES)
      f->images = copyImages(update->images, update->imageCount);
   if (c & CDMDPI_FORM_SET_CHOICES)
   {
      f->choices = choices;
      if (!offers(f->choices, f->value))
         f->value = choiceValues(f->choices).front();
   }
   if (c & CDMDPI_FORM_SET_VALUE)
      f->value = update->value ? update->value : "";
   return 1;
}

int MSGPIAPI fCloseForm(uint32_t)
{
   if (!g_form.open)
      return 0;
   g_form.open = false;
   g_form.closed = nullptr;
   return 1;
}

// ---- The fake bus ---------------------------------------------------------------------------

ColorizingDMDEditorAPI g_api{};
std::vector<std::string> g_msgNames;
struct Subscription
{
   unsigned int msgId;
   msgpi_msg_callback callback;
   void* userData;
};
std::vector<Subscription> g_subscriptions;

unsigned int MSGPIAPI bGetMsgID(const char* nameSpace, const char* name)
{
   const std::string full = std::string(nameSpace) + "/" + name;
   for (size_t i = 0; i < g_msgNames.size(); ++i)
      if (g_msgNames[i] == full)
         return static_cast<unsigned int>(i + 1);
   g_msgNames.push_back(full);
   return static_cast<unsigned int>(g_msgNames.size());
}
unsigned int msgIdOf(const char* name) { return bGetMsgID(CDMDPI_NAMESPACE, name); }
void MSGPIAPI bBroadcast(const uint32_t, const unsigned int msgId, void* data)
{
   if (msgId == msgIdOf(CDMDPI_MSG_GET_API))
      *static_cast<const ColorizingDMDEditorAPI**>(data) = &g_api;
}
void MSGPIAPI bRelease(const unsigned int) {}
void MSGPIAPI bSubscribe(const uint32_t, const unsigned int msgId, const msgpi_msg_callback callback, void* userData)
{
   g_subscriptions.push_back({msgId, callback, userData});
}
void MSGPIAPI bUnsubscribe(const unsigned int msgId, const msgpi_msg_callback callback, void* userData)
{
   g_subscriptions.erase(std::remove_if(g_subscriptions.begin(), g_subscriptions.end(),
                                        [&](const Subscription& s) {
                                           return s.msgId == msgId && s.callback == callback && s.userData == userData;
                                        }),
                         g_subscriptions.end());
}
MsgPluginAPI g_bus{};

// The editor broadcasts one of its events.
void fireEvent(const char* name)
{
   const unsigned int id = msgIdOf(name);
   const std::vector<Subscription> subscriptions = g_subscriptions;
   for (const Subscription& s : subscriptions)
      if (s.msgId == id)
         s.callback(id, s.userData, nullptr);
}

std::string g_language = "en"; // what the fake editor's GetLanguage says; empty: it does not know

int MSGPIAPI fGetLanguage(char* code, uint32_t capacity)
{
   if (g_language.empty())
      return 0;
   const uint32_t size = static_cast<uint32_t>(g_language.size() + 1);
   if (capacity >= size)
      std::memcpy(code, g_language.c_str(), size);
   return static_cast<int>(size);
}

int MSGPIAPI fIsDarkTheme() { return 1; }

void setUpApi()
{
   g_api.version = 12;
   g_api.GetLanguage = fGetLanguage;
   g_api.IsDarkTheme = fIsDarkTheme;
   g_api.GetProjectInfo = fGetProjectInfo;
   g_api.GetFrameInfo = fGetFrameInfo;
   g_api.GetOriginalFrame = fGetOriginalFrame;
   g_api.GetColorizedFrame = fGetColorizedFrame;
   g_api.GetDynamicMask = fGetDynamicMask;
   g_api.GetDynamicColors = fGetDynamicColors;
   g_api.GetCurrentFrame = fGetCurrentFrame;
   g_api.GetSelectedFrames = fGetSelectedFrames;
   g_api.ShowFrames = fShowFrames;
   g_api.AddMenuEntry = fAddMenuEntry;
   g_api.RemoveMenuEntries = fRemoveMenuEntries;
   g_api.ShowMessage = fShowMessage;
   g_api.ShowStatus = fShowStatus;
   g_api.GetSetting = fGetSetting;
   g_api.SetSetting = fSetSetting;
   g_api.BeginEdit = fBeginEdit;
   g_api.EndEdit = fEndEdit;
   g_api.AddFrame = fAddFrame;
   g_api.SetFramePicture = fSetFramePicture;
   g_api.SetDynamicZones = fSetDynamicZones;
   g_api.SetDynamicColors = fSetDynamicColors;
   g_api.ClearColorRotations = fClearColorRotations;
   g_api.SetFrameComparison = fSetFrameComparison;
   g_api.MoveFrames = fMoveFrames;
   g_api.DeleteFrames = fDeleteFrames;
   g_api.ShowLiveForm = fShowLiveForm;
   g_api.GetFormValue = fGetFormValue;
   g_api.SetFormField = fSetFormField;
   g_api.CloseForm = fCloseForm;
   g_api.CreateScene = fCreateScene;
   g_api.SetFrameScene = fSetFrameScene;
   g_bus.version = 1;
   g_bus.GetMsgID = bGetMsgID;
   g_bus.BroadcastMsg = bBroadcast;
   g_bus.ReleaseMsgID = bRelease;
   g_bus.SubscribeMsg = bSubscribe;
   g_bus.UnsubscribeMsg = bUnsubscribe;
}

void resetProject(uint32_t w, uint32_t h, bool withZones)
{
   g_w = w;
   g_h = h;
   g_frames.clear();
   g_frames.push_back(makeSourceFrame(withZones));
   g_frames.push_back(makeSourceFrame(withZones));
   g_scenes.clear();
   g_inBatch = false;
   g_current = 0;
   g_selected.clear();
   g_settings.clear();
   g_messages.clear();
   g_status.clear();
   g_shown.clear();
   g_showCalls = 0;
   g_endEditApplied = 0;
   g_addFrameLimit = -1;
   g_refuseMove = false;
   g_refuseScene = false;
   g_form = Form{};
   g_formsOpened = 0;
}

// ---- Scripting the window -------------------------------------------------------------------

// The author picks a value in a field of the open window: the editor shows it and tells the
// plugin.
void change(const char* key, const std::string& value)
{
   Form::Field* f = fieldOf(key);
   CHECK(g_form.open && f != nullptr);
   if (!g_form.open || !f)
      return;
   CHECK(f->visible && f->enabled);
   if (isChoice(f->kind))
   {
      if (!offers(f->choices, value))
         std::printf("%s does not offer %s\n", key, value.c_str());
      CHECK(offers(f->choices, value));
   }
   f->value = value;
   if (g_form.changed)
      g_form.changed(g_form.userData, key);
}

// The author closes the window: with Apply (the values are kept as settings) or Cancel.
void closeForm(bool accepted)
{
   CHECK(g_form.open);
   g_form.open = false;
   if (accepted)
      for (const auto& f : g_form.fields)
         if (!f.key.empty() && f.kind != CDMDPI_FIELD_NOTE && f.kind != CDMDPI_FIELD_IMAGE)
            g_settings[f.key] = f.value;
   cdmdpi_form_closed_callback closed = g_form.closed;
   if (closed)
      closed(g_form.userData, accepted ? CDMDPI_FORM_ACCEPTED : CDMDPI_FORM_CANCELLED);
   g_form.closed = nullptr;
}

void openMenu()
{
   CHECK(g_menuCallback != nullptr);
   g_menuCallback(nullptr);
}

// Runs the whole flow. option may be null for a transition without options; frames may be
// negative to take whatever the window shows.
bool runFlow(const char* transition, const char* option, long frames, const char* placement = "end",
             const char* target = "black")
{
   openMenu();
   if (!g_form.open)
      return false;
   CHECK(g_form.accept == "Apply");
   change("transition", transition);
   if (option)
      change("option", option);
   change("target", target);
   if (frames >= 0)
      change("frames", std::to_string(frames));
   change("placement", placement);
   closeForm(true);
   return true;
}
// ---- Helpers for the effects ----------------------------------------------------------------

struct Case
{
   const transitions::Transition* transition;
   const transitions::Option* option; // null without options
   bool toWhite = false;
   std::string name() const
   {
      return std::string(transition->label) + (option ? std::string(" / ") + option->label : std::string()) +
             (toWhite ? " to white" : " to black");
   }
};

// Every transition with every option; a fade with both targets, the others (whose geometry does
// not depend on the target) with both only when everyTarget is set.
std::vector<Case> allCases(bool everyTarget = false)
{
   std::vector<Case> cases;
   for (const auto& t : transitions::kTransitions)
   {
      const bool both = everyTarget || transitions::same(t.id, "fade");
      auto add = [&](const transitions::Option* option) {
         cases.push_back({&t, option, false});
         if (both)
            cases.push_back({&t, option, true});
      };
      if (t.optionCount == 0)
         add(nullptr);
      for (size_t i = 0; i < t.optionCount; ++i)
         add(&t.options[i]);
   }
   return cases;
}

transitions::SourceInfo makeInfo(uint32_t w, uint32_t h, bool toWhite = false)
{
   transitions::SourceInfo info;
   info.toWhite = toWhite;
   info.width = w;
   info.height = h;
   info.colorCount = kColors;
   info.brightestShade = 3;
   info.darkestShade = 1;
   for (uint32_t s = 1; s <= 3; ++s)
      for (uint32_t k = 0; k < 5; ++k)
         info.colors.push_back(static_cast<uint16_t>(0x0841 * (1 + s) + k * 0x0020));
   // Two pictures for the pixel effects: an oval with a ring (shades 3 and 2) on a background of
   // shade 1, and a rectangle with a bar, in other places.
   info.shades.assign(static_cast<size_t>(w) * h, 1);
   info.shadesSecond.assign(static_cast<size_t>(w) * h, 1);
   for (uint32_t y = 0; y < h; ++y)
      for (uint32_t x = 0; x < w; ++x)
      {
         const double dx = (x - w * 0.4) / (w * 0.25), dy = (y - h * 0.5) / (h * 0.4);
         const double d = dx * dx + dy * dy;
         if (d < 0.5)
            info.shades[static_cast<size_t>(y) * w + x] = 3;
         else if (d < 1.0)
            info.shades[static_cast<size_t>(y) * w + x] = 2;
         if (x > w * 0.55 && x < w * 0.9 && y > h * 0.2 && y < h * 0.8)
            info.shadesSecond[static_cast<size_t>(y) * w + x] = (y > h * 0.45 && y < h * 0.55) ? 3 : 2;
      }
   return info;
}

std::unique_ptr<transitions::Effect> makeEffect(const Case& c, uint32_t w, uint32_t h)
{
   transitions::SourceInfo info = makeInfo(w, h, c.toWhite);
   info.toFrame = c.transition->needsFrame;
   return c.transition->make(c.option ? c.option->id : "", info);
}

bool isFade(const Case& c) { return transitions::same(c.transition->id, "fade"); }

// What the frame at `step` of `steps` shows: for the moving effects the rules, for the fade the
// colors of a few shades and colors.
std::vector<uint64_t> signatureOf(const transitions::Effect& e, uint32_t w, uint32_t h, uint32_t step, uint32_t steps)
{
   std::vector<uint64_t> sig;
   if (e.changesColors())
   {
      for (uint32_t s = 0; s < kColors; ++s)
         sig.push_back(e.shade(static_cast<uint8_t>(s), step, steps));
      for (uint32_t s = 1; s <= 3; ++s)
         for (uint32_t k = 0; k < 5; ++k)
            sig.push_back(e.color(static_cast<uint16_t>(0x0841 * (1 + s) + k * 0x0020), step, steps));
      return sig;
   }
   for (uint32_t y = 0; y < h; ++y)
      for (uint32_t x = 0; x < w; ++x)
      {
         const transitions::Rule r = e.rule(x, y, step, steps);
         sig.push_back(r.visible ? (1ull << 40) | (r.second ? 1ull << 41 : 0ull) | (static_cast<uint64_t>(r.x) << 20) | r.y
                                : 0ull);
      }
   return sig;
}

size_t visibleCount(const transitions::Effect& e, uint32_t w, uint32_t h, uint32_t step, uint32_t steps)
{
   size_t n = 0;
   for (uint32_t y = 0; y < h; ++y)
      for (uint32_t x = 0; x < w; ++x)
         n += e.rule(x, y, step, steps).visible ? 1 : 0;
   return n;
}

// ---- Effect tests ---------------------------------------------------------------------------

void testRegistry()
{
   CHECK(sizeof(transitions::kTransitions) / sizeof(transitions::kTransitions[0]) == transitions::kTransitionCount);
   for (const auto& t : transitions::kTransitions)
   {
      CHECK(transitions::findTransition(t.id) == &t);
      for (size_t i = 0; i < t.optionCount; ++i)
         CHECK(transitions::findOption(t, t.options[i].id) == &t.options[i]);
      if (t.optionCount)
         CHECK(transitions::findOption(t, "no-such-option") == &t.options[0]);
      else
         CHECK(transitions::findOption(t, "x") == nullptr);
   }
   CHECK(transitions::findTransition("nope") == nullptr);
   // Alphabetical, and one Split with both orientations.
   for (size_t i = 1; i < transitions::kTransitionCount; ++i)
      CHECK(std::string(transitions::kTransitions[i - 1].label) < std::string(transitions::kTransitions[i].label));
   CHECK(transitions::findTransition("split_vertical") == nullptr);
   CHECK(transitions::findTransition("split_horizontal") == nullptr);
   const transitions::Transition* split = transitions::findTransition("split");
   CHECK(split && split->optionCount == 2 && std::string(split->options[0].label) == "Horizontal" &&
         std::string(split->options[1].label) == "Vertical");
   CHECK(transitions::findTransition("") == nullptr);
}

void testBayerIsAPermutation()
{
   std::set<uint32_t> seen;
   for (uint32_t y = 0; y < 16; ++y)
      for (uint32_t x = 0; x < 16; ++x)
         seen.insert(transitions::bayer16(x, y));
   CHECK(seen.size() == 256);
   CHECK(*seen.begin() == 0 && *seen.rbegin() == 255);
}

void testEveryEffectOnEverySize()
{
   const uint32_t sizes[][2] = {{128, 32}, {192, 64}, {256, 64}, {129, 33}};
   for (const auto& sz : sizes)
      for (const Case& c : allCases())
      {
         const uint32_t w = sz[0], h = sz[1];
         auto e = makeEffect(c, w, h);
         const uint32_t most = e->maxSteps();
         if (most < 1)
         {
            std::printf("no steps for %s on %ux%u\n", c.name().c_str(), w, h);
            CHECK(most >= 1);
            continue;
         }
         CHECK(most <= transitions::kMaxSteps);
         CHECK(e->limitReason() && *e->limitReason());

         // All frames at the maximum differ from each other.
         std::set<std::vector<uint64_t>> seen;
         for (uint32_t step = 1; step <= most; ++step)
            seen.insert(signatureOf(*e, w, h, step, most));
         if (seen.size() != most)
         {
            std::printf("repeated frames: %s on %ux%u, %zu different of %u\n", c.name().c_str(), w, h, seen.size(), most);
            CHECK(seen.size() == most);
         }

         // Fewer frames than the maximum differ as well (the window offers every number).
         for (uint32_t steps : {1u, 2u, std::max(1u, most / 2), most})
         {
            std::set<std::vector<uint64_t>> s;
            for (uint32_t step = 1; step <= steps; ++step)
               s.insert(signatureOf(*e, w, h, step, steps));
            if (s.size() != steps)
            {
               std::printf("repeated frames: %s on %ux%u with %u frames\n", c.name().c_str(), w, h, steps);
               CHECK(s.size() == steps);
            }
         }

         // The maximum is tight: one more and a frame repeats (when 255 does not cap it).
         if (most < transitions::kMaxSteps && !e->movesPixels())
         {
            std::set<std::vector<uint64_t>> s;
            for (uint32_t step = 1; step <= most + 1; ++step)
               s.insert(signatureOf(*e, w, h, step, most + 1));
            if (s.size() == most + 1)
               std::printf("maximum not tight: %s on %ux%u (%u)\n", c.name().c_str(), w, h, most);
            CHECK(s.size() < most + 1);
         }

         // No frame equals the starting frame, and the last frame is not completely black.
         if (!isFade(c))
         {
            std::vector<uint64_t> identity;
            for (uint32_t y = 0; y < h; ++y)
               for (uint32_t x = 0; x < w; ++x)
                  identity.push_back((1ull << 40) | (static_cast<uint64_t>(x) << 20) | y);
            for (uint32_t step = 1; step <= most; ++step)
               CHECK(signatureOf(*e, w, h, step, most) != identity);
            CHECK(visibleCount(*e, w, h, most, most) > 0);
            // Pixels only leave: strictly fewer visible pixels with every step (but not for the
            // effects that keep every pixel on the screen: all of them stay).
            size_t before = static_cast<size_t>(w) * h;
            for (uint32_t step = 1; step <= most && !e->movesPixels(); ++step)
            {
               const size_t now = visibleCount(*e, w, h, step, most);
               if (e->takesPixelsAway())
                  CHECK(now < before);
               else
                  CHECK(now == before);
               before = now;
            }
            // A visible pixel comes from inside the frame.
            for (uint32_t y = 0; y < h; ++y)
               for (uint32_t x = 0; x < w; ++x)
               {
                  const auto r = e->rule(x, y, most, most);
                  if (r.visible)
                  {
                     CHECK(r.x < w && r.y < h);
                  }
               }
         }
      }
}

void testDissolveSpecifics()
{
   const uint32_t w = 128, h = 32;
   // Orderly keeps one pixel in 256 at the last step.
   {
      auto e = transitions::findTransition("dissolve")->make("orderly", makeInfo(w, h));
      CHECK(visibleCount(*e, w, h, 255, 255) >= w * h / 256);
      // A pixel that is gone stays gone.
      for (uint32_t step = 1; step < 20; ++step)
         for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
               if (!e->rule(x, y, step, 20).visible)
                  CHECK(!e->rule(x, y, step + 1, 20).visible);
   }
   // Random: the same every time, and spread over the picture.
   {
      auto a = transitions::findTransition("dissolve")->make("random", makeInfo(w, h));
      auto b = transitions::findTransition("dissolve")->make("random", makeInfo(w, h));
      CHECK(signatureOf(*a, w, h, 5, 10) == signatureOf(*b, w, h, 5, 10));
      size_t quad[4] = {0, 0, 0, 0};
      for (uint32_t y = 0; y < h; ++y)
         for (uint32_t x = 0; x < w; ++x)
            if (!a->rule(x, y, 5, 10).visible)
               ++quad[(y >= h / 2) * 2 + (x >= w / 2)];
      for (size_t q : quad)
      {
         const double share = static_cast<double>(q) / (w * h / 4.0);
         CHECK(share > 0.35 && share < 0.65);
      }
   }
   // Centre-Out hides the pixels near the centre first, Edge-In the ones at the edges first.
   for (const char* kind : {"centre_out", "edge_in"})
   {
      auto e = transitions::findTransition("wipe")->make(kind, makeInfo(w, h));
      auto dist = [&](uint32_t x, uint32_t y) {
         const double dx = (x + 0.5 - w / 2.0) / (w / 2.0), dy = (y + 0.5 - h / 2.0) / (h / 2.0);
         return dx * dx + dy * dy;
      };
      for (uint32_t step : {2u, 5u, 8u})
      {
         double hiddenMax = -1, hiddenMin = 1e9, shownMax = -1, shownMin = 1e9;
         for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
            {
               const double d = dist(x, y);
               if (e->rule(x, y, step, 10).visible)
                  shownMax = std::max(shownMax, d), shownMin = std::min(shownMin, d);
               else
                  hiddenMax = std::max(hiddenMax, d), hiddenMin = std::min(hiddenMin, d);
            }
         if (std::strcmp(kind, "centre_out") == 0)
            CHECK(hiddenMax <= shownMin + 0.02);
         else
            CHECK(hiddenMin >= shownMax - 0.02);
      }
   }
   // Spiral: gone pixels stay gone, and the first ones to go are next to each other.
   {
      auto e = transitions::findTransition("wipe")->make("spiral", makeInfo(w, h));
      for (uint32_t step = 1; step < 20; ++step)
         for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
               if (!e->rule(x, y, step, 20).visible)
                  CHECK(!e->rule(x, y, step + 1, 20).visible);
      // The gone region at a small step is one connected piece (4-neighbour flood fill).
      std::vector<uint8_t> gone(w * h);
      size_t total = 0, start = 0;
      for (uint32_t y = 0; y < h; ++y)
         for (uint32_t x = 0; x < w; ++x)
            if (!e->rule(x, y, 2, 40).visible)
               gone[y * w + x] = 1, ++total, start = y * w + x;
      CHECK(total > 0);
      std::vector<size_t> stack{start};
      std::vector<uint8_t> done(w * h);
      size_t reached = 0;
      done[start] = 1;
      while (!stack.empty())
      {
         const size_t i = stack.back();
         stack.pop_back();
         ++reached;
         const uint32_t x = static_cast<uint32_t>(i % w), y = static_cast<uint32_t>(i / w);
         const int nx[4] = {int(x) - 1, int(x) + 1, int(x), int(x)}, ny[4] = {int(y), int(y), int(y) - 1, int(y) + 1};
         for (int k = 0; k < 4; ++k)
            if (nx[k] >= 0 && ny[k] >= 0 && nx[k] < int(w) && ny[k] < int(h))
            {
               const size_t j = size_t(ny[k]) * w + size_t(nx[k]);
               if (gone[j] && !done[j])
                  done[j] = 1, stack.push_back(j);
            }
      }
      CHECK(reached == total);
   }
}

transitions::Rule ruleOf(const char* transition, const char* option, uint32_t x, uint32_t y, uint32_t step, uint32_t steps,
                         uint32_t w = 128, uint32_t h = 32)
{
   return transitions::findTransition(transition)->make(option, makeInfo(w, h))->rule(x, y, step, steps);
}

void testMovingSpecifics()
{
   const uint32_t w = 128, h = 32;
   // Slide: the picture moves whole; a visible pixel comes from the same row (left/right) or column.
   for (uint32_t step = 1; step <= 10; ++step)
   {
      for (uint32_t y = 0; y < h; ++y)
         for (uint32_t x = 0; x < w; ++x)
         {
            auto l = ruleOf("slide", "left", x, y, step, 10);
            auto r = ruleOf("slide", "right", x, y, step, 10);
            auto u = ruleOf("slide", "up", x, y, step, 10);
            auto d = ruleOf("slide", "down", x, y, step, 10);
            if (l.visible) { CHECK(l.y == y && l.x > x); }
            if (r.visible) { CHECK(r.y == y && r.x < x); }
            if (u.visible) { CHECK(u.x == x && u.y > y); }
            if (d.visible) { CHECK(d.x == x && d.y < y); }
         }
   }
   // The offset between where a pixel is and where it comes from is the same for every pixel.
   {
      auto a = ruleOf("slide", "left", 3, 4, 5, 10);
      auto b = ruleOf("slide", "left", 20, 9, 5, 10);
      CHECK(a.visible && b.visible && a.x - 3 == b.x - 20);
   }
   // Wipe: a pixel that shows is the same pixel; the bar covers the side it moves from.
   for (uint32_t step = 1; step <= 10; ++step)
      for (uint32_t y = 0; y < h; ++y)
         for (uint32_t x = 0; x < w; ++x)
            for (const char* dir : {"left", "right", "up", "down"})
            {
               auto r = ruleOf("wipe", dir, x, y, step, 10);
               if (r.visible)
                  CHECK(r.x == x && r.y == y);
            }
   // The bar grows: a row-wise check on the left wipe, and the right wipe is its mirror.
   {
      size_t leftEdgeHidden = 0, rightEdgeHidden = 0;
      for (uint32_t x = 0; x < w; ++x)
      {
         leftEdgeHidden += !ruleOf("wipe", "left", x, 0, 5, 10).visible;
         rightEdgeHidden += !ruleOf("wipe", "right", x, 0, 5, 10).visible;
      }
      CHECK(leftEdgeHidden == rightEdgeHidden && leftEdgeHidden > 0);
      CHECK(!ruleOf("wipe", "left", w - 1, 0, 5, 10).visible);  // the bar sits at the far (right) side
      CHECK(ruleOf("wipe", "left", 0, 0, 5, 10).visible);
      CHECK(!ruleOf("wipe", "right", 0, 0, 5, 10).visible);
      CHECK(ruleOf("wipe", "right", w - 1, 0, 5, 10).visible);
      CHECK(!ruleOf("wipe", "up", 0, h - 1, 5, 10).visible);
      CHECK(ruleOf("wipe", "up", 0, 0, 5, 10).visible);
      CHECK(!ruleOf("wipe", "down", 0, 0, 5, 10).visible);
      CHECK(ruleOf("wipe", "down", 0, h - 1, 5, 10).visible);
   }
   // Alternating slide: odd rows (1st, 3rd ...) go left, even rows right; columns up and down.
   for (uint32_t step = 1; step <= 6; ++step)
      for (uint32_t y = 0; y < h; ++y)
         for (uint32_t x = 0; x < w; ++x)
         {
            auto hz = ruleOf("alternating_slide", "horizontal", x, y, step, 6);
            if (hz.visible)
            {
               CHECK(hz.y == y);
               CHECK((y % 2 == 0) ? hz.x > x : hz.x < x);
            }
            auto vt = ruleOf("alternating_slide", "vertical", x, y, step, 6);
            if (vt.visible)
            {
               CHECK(vt.x == x);
               CHECK((x % 2 == 0) ? vt.y > y : vt.y < y);
            }
         }
   // Split: the left half moves left, the right half moves right, rows stay.
   for (uint32_t y = 0; y < h; ++y)
      for (uint32_t x = 0; x < w; ++x)
      {
         auto r = ruleOf("split", "vertical", x, y, 4, 10);
         if (r.visible)
         {
            CHECK(r.y == y);
            CHECK(x < w / 2 ? r.x > x : r.x < x);
         }
         auto q = ruleOf("split", "horizontal", x, y, 4, 10);
         if (q.visible)
         {
            CHECK(q.x == x);
            CHECK(y < h / 2 ? q.y > y : q.y < y);
         }
      }
   // Blinds: the hidden bars repeat at a regular interval and grow.
   for (const char* o : {"horizontal", "vertical"})
   {
      const bool rows = std::strcmp(o, "horizontal") == 0;
      const uint32_t n = rows ? h : w;
      auto e = transitions::findTransition("blinds")->make(o, makeInfo(w, h));
      const uint32_t most = e->maxSteps();
      std::vector<uint32_t> hiddenPerStep;
      for (uint32_t step = 1; step <= most; ++step)
      {
         uint32_t hidden = 0;
         std::vector<uint8_t> line(n);
         for (uint32_t i = 0; i < n; ++i)
         {
            const bool v = rows ? e->rule(0, i, step, most).visible : e->rule(i, 0, step, most).visible;
            line[i] = v;
            hidden += !v;
         }
         hiddenPerStep.push_back(hidden);
         // Regular: the pattern repeats with the pitch (the line is a whole number of pitches here).
         uint32_t pitch = 0;
         for (uint32_t p = 1; p < n && !pitch; ++p)
         {
            bool ok = n % p == 0;
            for (uint32_t i = 0; ok && i + p < n; ++i)
               ok = line[i] == line[i + p];
            if (ok)
               pitch = p;
         }
         CHECK(pitch > 0 && pitch < n);
      }
      for (size_t i = 1; i < hiddenPerStep.size(); ++i)
         CHECK(hiddenPerStep[i] > hiddenPerStep[i - 1]);
   }
}

void testFadeSpecifics()
{
   for (const char* o : {"black", "white"})
   {
      const bool toWhite = std::strcmp(o, "white") == 0;
      auto e = transitions::findTransition("fade")->make("", makeInfo(128, 32, toWhite));
      CHECK(e->changesColors());
      // Geometry never changes.
      auto r = e->rule(3, 4, 2, 5);
      CHECK(r.visible && r.x == 3 && r.y == 4);
      const uint32_t most = e->maxSteps();
      auto lum = [](uint16_t c) { return int((c >> 11) & 31) + int((c >> 5) & 63) / 2 + int(c & 31); };
      const uint16_t sample = 0x0841 * 4 + 0x0020;
      int previous = lum(sample);
      for (uint32_t step = 1; step <= most; ++step)
      {
         const int now = lum(e->color(sample, step, most));
         if (toWhite)
            CHECK(now >= previous);
         else
            CHECK(now <= previous);
         previous = now;
      }
      // Almost black / almost white, never exactly.
      uint16_t brightest = 0, darkest = 0xFFFF;
      for (uint16_t c : makeInfo(128, 32).colors)
         if (lum(c) > lum(brightest)) brightest = c;
      for (uint16_t c : makeInfo(128, 32).colors)
         if (lum(c) < lum(darkest)) darkest = c;
      if (toWhite)
         CHECK(e->color(darkest, most, most) != 0xFFFF);
      else
         CHECK(e->color(brightest, most, most) != 0);
      if (toWhite)
         CHECK(lum(e->color(darkest, most, most)) > lum(darkest));
      else
         CHECK(lum(e->color(brightest, most, most)) < lum(brightest));
      // An all-black picture cannot fade to black: no steps.
      transitions::SourceInfo black = makeInfo(128, 32);
      black.colors = {0};
      black.brightestShade = black.darkestShade = 0;
      if (!toWhite)
         CHECK(transitions::findTransition("fade")->make("", black)->maxSteps() == 0);
   }
}

void testPreviewSteps()
{
   CHECK((preview::previewSteps(1) == std::vector<uint32_t>{1}));
   CHECK((preview::previewSteps(2) == std::vector<uint32_t>{1, 2}));
   CHECK((preview::previewSteps(5) == std::vector<uint32_t>{2, 3, 4, 5}));
   CHECK((preview::previewSteps(255) == std::vector<uint32_t>{64, 128, 192, 255}));
   for (uint32_t steps = 1; steps <= 255; ++steps)
   {
      const auto shown = preview::previewSteps(steps);
      CHECK(!shown.empty() && shown.size() <= preview::kPreviewPictures && shown.back() == steps);
      for (size_t i = 1; i < shown.size(); ++i)
         CHECK(shown[i - 1] < shown[i]);
   }
   CHECK(preview::previewScale(128) == 2 && preview::previewScale(256) == 1);
}

// ---- Plugin tests ---------------------------------------------------------------------------

size_t countShade(const std::vector<uint8_t>& v, uint8_t shade)
{
   return static_cast<size_t>(std::count(v.begin(), v.end(), shade));
}

// The status line names the new frames ("frames 3 to 6", or "(frames 3 to 6)" for a scene).
bool statusFrames(uint32_t first, uint32_t last)
{
   return !g_status.empty() &&
          g_status.back().find("frames " + std::to_string(first) + " to " + std::to_string(last)) != std::string::npos;
}

void checkNewFrames(size_t added, bool zones)
{
   CHECK(g_frames.size() == 2 + added);
   CHECK(g_endEditApplied == 1);
   CHECK(g_showCalls == 0);
   for (size_t i = 2; i < g_frames.size(); ++i)
   {
      CHECK(g_frames[i].comparisonMask == CDMDPI_NONE);
      CHECK(g_frames[i].original.size() == static_cast<size_t>(g_w) * g_h);
      CHECK(g_frames[i].sd.size() == static_cast<size_t>(g_w) * g_h);
      if (zones)
         CHECK(g_frames[i].zones.size() == static_cast<size_t>(g_w) * g_h);
   }
   // The starting frame is never changed.
   CHECK(g_frames[0].original == makeSourceFrame(zones).original);
   CHECK(g_frames[0].sd == makeSourceFrame(zones).sd);
}

void testEveryTransitionThroughTheWindow(uint32_t w, uint32_t h, bool zones)
{
   for (const Case& c : allCases(true))
   {
      if (c.transition->needsFrame)
         continue; // goes to another frame only (testMorph)
      resetProject(w, h, zones);
      g_frames[0].comparisonMask = 3; // the source has a mask; the new frames must not
      // Dust has fewer frames on a busy picture; WPC is always 7.
      const uint32_t frames = transitions::same(c.transition->id, "dust") ? 3 : (c.option && transitions::same(c.option->id, "wpc") ? 7 : 5);
      if (!runFlow(c.transition->id, c.option ? c.option->id : nullptr, frames, "end", c.toWhite ? "white" : "black"))
      {
         std::printf("flow failed for %s on %ux%u\n", c.name().c_str(), w, h);
         CHECK(false);
         continue;
      }
      if (g_frames.size() != 2 + frames)
      {
         std::printf("wrong frame count for %s on %ux%u (messages: %s)\n", c.name().c_str(), w, h,
                     g_messages.empty() ? "none" : g_messages.back().c_str());
         CHECK(g_frames.size() == 2 + frames);
         continue;
      }
      checkNewFrames(frames, zones);
      CHECK(g_status.size() == 1 && g_status[0].find("added " + std::to_string(frames) + " frame(s)") != std::string::npos);
      // All new frames differ from one another and from the start, in both the shades and the colors.
      std::set<std::pair<std::vector<uint8_t>, std::vector<uint16_t>>> seen;
      for (size_t i = 2; i < g_frames.size(); ++i)
      {
         CHECK(g_frames[i].original != g_frames[0].original || g_frames[i].sd != g_frames[0].sd);
         seen.insert({g_frames[i].original, g_frames[i].sd});
      }
      CHECK(seen.size() == frames);
      // The last frame is not all black (shade 0 / color 0) unless it is a fade, which keeps
      // its geometry and so is never all black either.
      const Frame& last = g_frames.back();
      CHECK(countShade(last.original, 0) < last.original.size() || isFade(c));
      if (c.toWhite)
      {
         CHECK(std::count(last.sd.begin(), last.sd.end(), 0xFFFF) < static_cast<long>(last.sd.size()));
         // what a transition takes away is exactly RGB565 white (a fade changes the colors instead)
         if (!isFade(c) && makeEffect(c, w, h)->takesPixelsAway())
            CHECK(std::count(last.sd.begin(), last.sd.end(), 0xFFFF) > 0);
      }
      CHECK(std::count(last.sd.begin(), last.sd.end(), 0) < static_cast<long>(last.sd.size()));
      // Black pixels in the original are black in the picture, too, and out of the zones.
      if (!isFade(c))
         for (size_t i = 2; i < g_frames.size(); ++i)
            for (size_t p = 0; p < g_frames[i].original.size(); ++p)
            {
               if (!c.toWhite)
               {
                  const bool black = g_frames[i].sd[p] == 0;
                  CHECK(black == (g_frames[i].original[p] == 0));
                  if (zones && black)
                     CHECK(g_frames[i].zones[p] == 255);
               }
               else if (g_frames[i].sd[p] == 0xFFFF) // taken away: the brightest shade, in no zone
               {
                  CHECK(g_frames[i].original[p] == kColors - 1);
                  if (zones)
                     CHECK(g_frames[i].zones[p] == 255);
               }
               else
                  CHECK(g_frames[i].sd[p] != 0);
            }
      if (isFade(c) && zones)
         for (size_t i = 2; i < g_frames.size(); ++i)
         {
            CHECK(g_frames[i].rotations == 0);
            CHECK(g_frames[i].dynamicColors != g_frames[0].dynamicColors);
         }
      if (!isFade(c) && zones)
         for (size_t i = 2; i < g_frames.size(); ++i)
            CHECK(g_frames[i].rotations == 2); // other transitions leave the rotations alone
   }
}

void testTheWindow()
{
   resetProject(128, 32, false);
   openMenu();
   CHECK(g_form.open && g_form.accept == "Apply" && g_formsOpened == 1);

   // The transition list holds every transition, the first one chosen at first.
   const Form::Field* t = fieldOf("transition");
   CHECK(t && t->kind == CDMDPI_FIELD_RADIO);
   if (t)
   {
      const auto values = choiceValues(*t);
      CHECK(values.size() == transitions::kTransitionCount);
      for (size_t i = 0; i < values.size(); ++i)
         CHECK(values[i] == transitions::kTransitions[i].id);
      CHECK(t->value == transitions::kTransitions[0].id);
   }
   const Form::Field* source = fieldOf("source");
   CHECK(source && source->kind == CDMDPI_FIELD_NOTE && source->label.find("frame 0") != std::string::npos);

   // The option follows the transition: its label, its list, its help.
   change("transition", "dissolve");
   const Form::Field* o = fieldOf("option");
   CHECK(o && o->kind == CDMDPI_FIELD_RADIO && o->visible);
   // The blank line between the transition and its option shows with the option, not without.
   CHECK(fieldOf("gap") && fieldOf("gap")->kind == CDMDPI_FIELD_NOTE && fieldOf("gap")->visible);
   change("transition", "fade");
   CHECK(fieldOf("option") && !fieldOf("option")->visible && fieldOf("gap") && !fieldOf("gap")->visible);
   change("transition", "dissolve");
   CHECK(fieldOf("gap") && fieldOf("gap")->visible);
   if (o)
   {
      const std::vector<std::string> want = {"orderly", "random", "wpc"};
      CHECK(choiceValues(*o) == want);
      CHECK(o->label == "Dissolve Type" && o->value == "orderly");
      CHECK(o->help.empty() && fieldOf("transition")->help.find("Random") != std::string::npos);
   }
   // What the transition goes to: black or white, for every transition.
   const Form::Field* tg = fieldOf("target");
   CHECK(tg && tg->kind == CDMDPI_FIELD_RADIO && tg->label == "Transition to" && tg->value == "black");
   if (tg)
   {
      const std::vector<std::string> want = {"black", "white", "frame"};
      CHECK(choiceValues(*tg) == want);
   }
   change("option", "random");

   // The frames list: exactly 1 .. maxSteps, opened on 10.
   const Form::Field* f = fieldOf("frames");
   CHECK(f && f->kind == CDMDPI_FIELD_CHOICE); // too many numbers for radio buttons
   if (f)
   {
      const auto values = choiceValues(*f);
      CHECK(values.size() == 255);
      for (size_t i = 0; i < values.size(); ++i)
         CHECK(values[i] == std::to_string(i + 1));
      CHECK(f->value == "10");
   }

   // Another transition: its own options, and a frames list that keeps 10 where it can.
   change("transition", "split");
   CHECK(o && o->label == "Orientation" && (choiceValues(*o) == std::vector<std::string>{"horizontal", "vertical"}));
   CHECK(f && choiceValues(*f).size() == 15 && f->value == "10");
   change("frames", "15");
   change("option", "vertical");
   // Split Vertical makes more frames on 128 x 32 than Horizontal; 15 still works.
   CHECK(f && choiceValues(*f).size() > 15 && f->value == "15");
   change("option", "horizontal");
   CHECK(f && choiceValues(*f).size() == 15 && f->value == "15");

   // Where the new frames go: after, at the end, or as a scene on 128 x 32.
   const Form::Field* place = fieldOf("placement");
   CHECK(place && place->kind == CDMDPI_FIELD_RADIO && place->value == "after");
   if (place)
      CHECK((choiceValues(*place) == std::vector<std::string>{"after", "end", "scene"}));
   const Form::Field* duration = fieldOf("duration");
   CHECK(duration && duration->kind == CDMDPI_FIELD_TEXT && !duration->visible && duration->value == "50");
   change("placement", "scene");
   CHECK(duration && duration->visible);
   change("placement", "end");
   CHECK(duration && !duration->visible);

   // The preview: steps of the transition on the author's frame, as Apply draws them.
   const Form::Field* pictures = fieldOf("preview");
   CHECK(pictures && pictures->kind == CDMDPI_FIELD_IMAGE && pictures->visible);
   if (pictures)
   {
      CHECK(pictures->images.size() == 4);
      for (const auto& image : pictures->images)
         CHECK(image.width == 128 && image.height == 32 && image.scale == 2 && image.pixels.size() == 128u * 32u);
      CHECK(!pictures->images.empty() && pictures->images.back().caption == "Frame 15 of 15");
   }
   const std::vector<uint16_t> lastPreview = pictures && !pictures->images.empty() ? pictures->images.back().pixels
                                                                                   : std::vector<uint16_t>{};
   change("frames", "3");
   CHECK(pictures && pictures->images.size() == 3 && pictures->images.back().caption == "Frame 3 of 3");

   closeForm(true);
   CHECK(!g_form.open && g_frames.size() == 5);
   checkNewFrames(3, false);
   // What the preview showed for the last frame is what the last frame is.
   resetProject(128, 32, false);
   g_settings["transition"] = "split";
   g_settings["option_split"] = "horizontal";
   g_settings["frames"] = "15";
   g_settings["placement"] = "end";
   openMenu();
   closeForm(true);
   CHECK(g_frames.size() == 17 && g_frames.back().sd == lastPreview);
}

void testTheWindowRemembers()
{
   resetProject(128, 32, false);
   runFlow("wipe", "down", 4, "end");
   CHECK(g_frames.size() == 6);
   // The next time the window opens on the same choices.
   openMenu();
   CHECK(fieldOf("transition") && fieldOf("transition")->value == "wipe");
   CHECK(fieldOf("option") && fieldOf("option")->value == "down" && fieldOf("option")->label == "Wipe Type");
   CHECK(fieldOf("frames") && fieldOf("frames")->value == "4");
   CHECK(fieldOf("placement") && fieldOf("placement")->value == "end");
   // An option is remembered per transition.
   change("transition", "slide");
   change("option", "right");
   change("transition", "wipe");
   CHECK(fieldOf("option")->value == "down");
   change("transition", "slide");
   CHECK(fieldOf("option")->value == "right");
   closeForm(false);
}

void testFramesListMatchesTheMaximum()
{
   const uint32_t sizes[][2] = {{128, 32}, {192, 64}, {256, 64}};
   for (const auto& sz : sizes)
      for (const Case& c : allCases())
      {
         if (c.transition->needsFrame)
            continue;
         resetProject(sz[0], sz[1], false);
         openMenu();
         change("transition", c.transition->id);
         if (c.option)
            change("option", c.option->id);
         const Form::Field* f = fieldOf("frames");
         if (!f)
         {
            CHECK(false);
            continue;
         }
         auto e = makeEffect(c, sz[0], sz[1]);
         // The plugin's own effect sees the real frame, whose colors differ from makeInfo: only
         // the geometry-bound transitions have to agree exactly.
         if (!isFade(c) && !e->movesPixels())
            CHECK(choiceValues(*f).size() == e->maxSteps() - e->minSteps() + 1);
         CHECK(!choiceValues(*f).empty());
         closeForm(false);
      }
}

void testSavedFramesAreClamped()
{
   // The saved number is larger than this transition allows: the window opens on the most.
   resetProject(128, 32, false);
   g_settings["transition"] = "split";
   g_settings["option_split"] = "horizontal";
   g_settings["frames"] = "200";
   openMenu();
   const Form::Field* f = fieldOf("frames");
   CHECK(f && choiceValues(*f).size() == 15 && f->value == "15");
   closeForm(false);
   // And one saved as nonsense opens on a valid number.
   resetProject(128, 32, false);
   g_settings["transition"] = "split";
   g_settings["frames"] = "banana";
   openMenu();
   const Form::Field* g = fieldOf("frames");
   CHECK(g && std::atol(g->value.c_str()) >= 1 && std::atol(g->value.c_str()) <= 15);
   closeForm(false);
}

void testCancelChangesNothing()
{
   resetProject(128, 32, false);
   openMenu();
   change("transition", "dissolve");
   change("option", "random");
   closeForm(false);
   CHECK(g_frames.size() == 2);
   CHECK(g_endEditApplied == 0);
   CHECK(!g_form.open);
   CHECK(g_messages.empty());
   CHECK(g_settings.count("transition") == 0); // Cancel keeps no values
}

void testRefusalRollsBack()
{
   resetProject(128, 32, true);
   g_addFrameLimit = 3; // the fourth AddFrame is refused
   runFlow("dissolve", "orderly", 6);
   CHECK(g_frames.size() == 2);
   CHECK(g_endEditApplied == 0);
   CHECK(!g_inBatch);
   CHECK(g_messages.size() == 1 && g_messages[0].find("Nothing was changed") != std::string::npos);
   CHECK(g_status.empty());
}

void testTheWindowFollowsTheCurrentFrame()
{
   resetProject(128, 32, false);
   g_frames.push_back(makeSourceFrame(false));
   // Frame 2 is all black but for its left half: fewer lit pixels than frame 0.
   for (uint32_t y = 0; y < g_h; ++y)
      for (uint32_t x = g_w / 2; x < g_w; ++x)
      {
         g_frames[2].original[static_cast<size_t>(y) * g_w + x] = 0;
         g_frames[2].sd[static_cast<size_t>(y) * g_w + x] = 0;
      }
   openMenu();
   change("transition", "wipe");
   change("option", "left");
   change("frames", "4");
   change("placement", "after");
   const Form::Field* source = fieldOf("source");
   const Form::Field* pictures = fieldOf("preview");
   const auto before = pictures ? pictures->images.front().pixels : std::vector<uint16_t>{};

   // The author clicks frame 2 while the window is open.
   g_current = 2;
   fireEvent(CDMDPI_EVT_ON_SELECTION_CHANGED);
   CHECK(source && source->label.find("frame 2") != std::string::npos);
   CHECK(pictures && pictures->images.front().pixels != before);
   CHECK(fieldOf("frames")->value == "4"); // the choices stay

   // Then no frame: the window says so; Apply does nothing.
   g_current = CDMDPI_NONE;
   fireEvent(CDMDPI_EVT_ON_SELECTION_CHANGED);
   CHECK(source && source->label.find("Select the frame") != std::string::npos);
   CHECK(pictures && !pictures->visible);
   g_current = 2;
   fireEvent(CDMDPI_EVT_ON_SELECTION_CHANGED);
   CHECK(pictures && pictures->visible);

   // The project changes while the window is open: that is no reason to refuse any more.
   ++g_revision;
   fireEvent(CDMDPI_EVT_ON_PROJECT_CHANGED);
   closeForm(true);
   CHECK(g_frames.size() == 7 && g_messages.empty());
   CHECK(g_showCalls == 0 && statusFrames(3, 6));
   CHECK(g_frames[3].original != g_frames[0].original);
   // Events after the window closed change nothing.
   const size_t updates = static_cast<size_t>(g_fieldUpdates);
   fireEvent(CDMDPI_EVT_ON_SELECTION_CHANGED);
   CHECK(static_cast<size_t>(g_fieldUpdates) == updates);
}

void testNoFrameSelected()
{
   resetProject(128, 32, false);
   g_current = CDMDPI_NONE;
   openMenu();
   CHECK(!g_form.open);
   CHECK(g_messages.size() == 1);
   resetProject(128, 32, false);
   g_current = 99;
   openMenu();
   CHECK(!g_form.open);
   CHECK(g_messages.size() == 1);
}

void testOneWindowAtATime()
{
   resetProject(128, 32, false);
   openMenu();
   openMenu(); // the menu again while the window is open: nothing new
   CHECK(g_formsOpened == 1 && g_messages.empty());
   closeForm(false);
   openMenu();
   CHECK(g_formsOpened == 2);
   closeForm(false);
}

void testAllBlackFrameCannotFadeToBlack()
{
   resetProject(128, 32, false);
   for (auto& f : g_frames)
   {
      std::fill(f.original.begin(), f.original.end(), 0);
      std::fill(f.sd.begin(), f.sd.end(), 0);
   }
   openMenu();
   change("transition", "fade");
   change("target", "black");
   const Form::Field* f = fieldOf("frames");
   CHECK(f && (choiceValues(*f) == std::vector<std::string>{"0"}) && f->help.find("nothing") != std::string::npos);
   CHECK(fieldOf("preview") && !fieldOf("preview")->visible);
   closeForm(true);
   CHECK(g_messages.size() == 1);
   CHECK(g_frames.size() == 2);
}

void testBackgroundCaveat()
{
   resetProject(128, 32, false);
   g_frames[0].background = 1;
   runFlow("wipe", "left", 4);
   CHECK(g_frames.size() == 6);
   CHECK(g_messages.size() == 1 && g_messages[0].find("background") != std::string::npos);
}

void testNoRepeatedFramesInThePlugin()
{
   // Every frame count the window offers gives all-different frames: the maximum of two
   // geometry-bound transitions, through the whole plugin.
   for (const char* option : {"horizontal", "vertical"})
   {
      resetProject(128, 32, false);
      openMenu();
      change("transition", "split");
      change("option", option);
      const Form::Field* f = fieldOf("frames");
      const long most = f ? static_cast<long>(choiceValues(*f).size()) : 0;
      change("frames", std::to_string(most));
      change("placement", "end");
      closeForm(true);
      CHECK(g_frames.size() == static_cast<size_t>(2 + most));
      std::set<std::vector<uint8_t>> seen;
      for (size_t i = 2; i < g_frames.size(); ++i)
         seen.insert(g_frames[i].original);
      CHECK(seen.size() == static_cast<size_t>(most));
   }
}

// A frame the tests can recognise after it has moved.
void mark(size_t frame, uint32_t marker) { g_frames[frame].background = marker; }

size_t litPixels(const Frame& f) { return f.original.size() - countShade(f.original, 0); }

void testPlacementAfterTheSource()
{
   // Source in front: the new frames come between it and the frame that followed it.
   resetProject(128, 32, false);
   mark(1, 7);
   runFlow("dissolve", "orderly", 4, "after");
   CHECK(g_frames.size() == 6 && g_endEditApplied == 1);
   CHECK(g_frames[5].background == 7);
   CHECK(g_frames[0].background == CDMDPI_NONE);
   for (size_t i = 1; i <= 4; ++i)
   {
      CHECK(g_frames[i].background == CDMDPI_NONE);
      CHECK(g_frames[i].comparisonMask == CDMDPI_NONE);
      if (i > 1)
         CHECK(litPixels(g_frames[i]) < litPixels(g_frames[i - 1])); // in the order of the steps
   }
   CHECK(litPixels(g_frames[1]) < litPixels(g_frames[0]));
   CHECK(g_showCalls == 0 && statusFrames(1, 4));
   CHECK(g_status.size() == 1 && g_status[0].find("as frames 1 to 4") != std::string::npos);

   // Source in the middle.
   resetProject(128, 32, false);
   g_frames.push_back(makeSourceFrame(false));
   mark(2, 9);
   g_current = 1;
   runFlow("slide", "left", 3, "after");
   CHECK(g_frames.size() == 6);
   CHECK(g_frames[5].background == 9);
   CHECK(g_showCalls == 0 && statusFrames(2, 4));
   CHECK(g_frames[2].original != g_frames[1].original);

   // Source last: the new frames already are behind it, nothing has to move.
   resetProject(128, 32, false);
   g_current = 1;
   runFlow("wipe", "up", 3, "after");
   CHECK(g_frames.size() == 5 && g_endEditApplied == 1);
   CHECK(g_showCalls == 0 && statusFrames(2, 4));

   // The default of the window is "after".
   resetProject(128, 32, false);
   mark(1, 7);
   openMenu();
   change("transition", "fade");
   change("target", "black");
   change("frames", "3");
   closeForm(true);
   CHECK(g_frames.size() == 5 && g_frames[4].background == 7);
}

void testPlacementAtTheEnd()
{
   resetProject(128, 32, false);
   mark(1, 7);
   runFlow("blinds", "vertical", 3, "end");
   CHECK(g_frames.size() == 5);
   CHECK(g_frames[1].background == 7);
   CHECK(g_showCalls == 0 && statusFrames(2, 4));
   CHECK(g_status.size() == 1 && g_status[0].find("as frames 2 to 4") != std::string::npos);
}

void testRefusedMoveRollsBack()
{
   resetProject(128, 32, true);
   g_refuseMove = true;
   runFlow("dissolve", "random", 3, "after");
   CHECK(g_frames.size() == 2);
   CHECK(g_endEditApplied == 0 && !g_inBatch);
   CHECK(g_messages.size() == 1 && g_messages[0].find("Nothing was changed") != std::string::npos);
   CHECK(g_status.empty());
}


// ---- Transitions to another frame -----------------------------------------------------------

// A second frame that differs from the first everywhere (the shades, the pictures), with dynamic
// zones in sets 0 and 1 (the first frame's zones use sets 0 .. 3): set 0 has other colors than
// the first frame's set 0 (a clash), set 1 the same colors (no clash).
Frame makeSecondFrame(bool zones)
{
   Frame f = makeSourceFrame(zones);
   for (uint32_t y = 0; y < g_h; ++y)
      for (uint32_t x = 0; x < g_w; ++x)
      {
         const size_t i = static_cast<size_t>(y) * g_w + x;
         f.original[i] = static_cast<uint8_t>(1 + (x + 2 * y + 1) % 3);
         f.sd[i] = static_cast<uint16_t>(0x0841 * (1 + f.original[i]) + (i % 7) * 0x0020 + 0x4000);
      }
   if (zones)
   {
      for (size_t i = 0; i < f.zones.size(); ++i)
         f.zones[i] = static_cast<uint8_t>(i % 2);
      for (size_t k = 0; k < kColors; ++k)
         f.dynamicColors[0 * kColors + k] = static_cast<uint16_t>(0xF800 - 0x0800 * k); // reds, not the first frame's
   }
   return f;
}

void selectTwo(uint32_t start, uint32_t end)
{
   g_current = start;
   g_selected = {std::min(start, end), std::max(start, end)};
}

void testAnotherFrame()
{
   // The choice, the note and the list follow the selection.
   resetProject(128, 32, true);
   g_frames[1] = makeSecondFrame(true);
   selectTwo(0, 1);
   openMenu();
   change("target", "frame");
   CHECK(fieldOf("source") && fieldOf("source")->label.find("From frame 0 to frame 1") != std::string::npos);
   change("transition", "wipe");
   change("option", "left");
   const Form::Field* f = fieldOf("frames");
   CHECK(f && choiceValues(*f).size() >= 10);
   change("frames", "3");
   change("placement", "end");
   CHECK(fieldOf("preview") && fieldOf("preview")->visible && fieldOf("preview")->images.size() == 3);
   closeForm(true);
   CHECK(g_endEditApplied == 1 && g_frames.size() == 5);
   CHECK(g_status.size() == 1 && g_status[0].find("Wipe (Left) to frame 1") != std::string::npos);
   // The clash is reported: set 1 (human) holds different colors in both frames.
   CHECK(g_messages.size() == 1 && g_messages[0].find("dynamic set 1") != std::string::npos &&
         g_messages[0].find("dynamic set 5") != std::string::npos);
   const Frame& a = g_frames[0];
   const Frame& b = g_frames[1];
   long fromSecond[3] = {0, 0, 0}, fromFirst[3] = {0, 0, 0};
   for (int k = 0; k < 3 && g_frames.size() == 5; ++k)
   {
      const Frame& n = g_frames[2 + k];
      for (size_t p = 0; p < n.sd.size(); ++p)
      {
         if (n.sd[p] == a.sd[p])
         {
            ++fromFirst[k];
            CHECK(n.original[p] == a.original[p] && n.zones[p] == a.zones[p]);
         }
         else
         {
            ++fromSecond[k];
            CHECK(n.sd[p] == b.sd[p] && n.original[p] == b.original[p]);
            // Its zone: the second frame's, with its set 0 (clashing) moved to the first free set (4).
            const uint8_t want = b.zones[p] == 0 ? 4 : b.zones[p];
            CHECK(n.zones[p] == want);
         }
      }
      // Set 4 has the second frame's set 0 colors; set 0 is still the first frame's; no rotations.
      for (uint32_t c = 0; c < kColors; ++c)
      {
         CHECK(n.dynamicColors[4 * kColors + c] == b.dynamicColors[0 * kColors + c]);
         CHECK(n.dynamicColors[0 * kColors + c] == a.dynamicColors[0 * kColors + c]);
      }
      CHECK(n.rotations == 0);
   }
   CHECK(fromSecond[0] > 0 && fromSecond[1] > fromSecond[0] && fromSecond[2] > fromSecond[1]);
   CHECK(fromFirst[2] > 0 && fromFirst[0] > fromFirst[1] && fromFirst[1] > fromFirst[2]); // never all of the second

   // The same colors in the same set do not clash: no set is moved, no message.
   resetProject(128, 32, true);
   g_frames[1] = makeSecondFrame(true);
   for (size_t k = 0; k < kColors; ++k)
      g_frames[1].dynamicColors[k] = g_frames[0].dynamicColors[k];
   selectTwo(0, 1);
   openMenu();
   change("target", "frame");
   change("transition", "wipe");
   change("option", "left");
   change("frames", "2");
   change("placement", "end");
   closeForm(true);
   CHECK(g_frames.size() == 4 && g_messages.empty());
   for (size_t p = 0; g_frames.size() == 4 && p < g_frames[3].sd.size(); ++p)
      if (g_frames[3].sd[p] == g_frames[1].sd[p])
         CHECK(g_frames[3].zones[p] == g_frames[1].zones[p]);

   // No dynamic set left for a clashing one: those zones are not in the new frames, and it is said.
   resetProject(128, 32, true);
   for (size_t i = 0; i < g_frames[0].zones.size(); ++i)
      g_frames[0].zones[i] = static_cast<uint8_t>(i % 32);
   g_frames[1] = makeSecondFrame(true);
   selectTwo(0, 1);
   openMenu();
   change("target", "frame");
   change("transition", "wipe");
   change("option", "left");
   change("frames", "2");
   change("placement", "end");
   closeForm(true);
   CHECK(g_frames.size() == 4 && g_messages.size() == 1 && g_messages[0].find("No dynamic set was free") != std::string::npos);
   for (size_t p = 0; g_frames.size() == 4 && p < g_frames[3].sd.size(); ++p)
      if (g_frames[3].sd[p] == g_frames[1].sd[p] && g_frames[1].zones[p] == 0)
         CHECK(g_frames[3].zones[p] == 255);

   // A cross-fade blends the pictures; the zones change over at the halfway point.
   resetProject(128, 32, true);
   g_frames[1] = makeSecondFrame(true);
   selectTwo(0, 1);
   openMenu();
   change("target", "frame");
   change("transition", "fade");
   const Form::Field* ff = fieldOf("frames");
   CHECK(ff && choiceValues(*ff).size() >= 3);
   change("frames", "3");
   change("placement", "end");
   closeForm(true);
   CHECK(g_frames.size() == 5);
   for (int k = 0; k < 3 && g_frames.size() == 5; ++k)
   {
      const Frame& n = g_frames[2 + k];
      long blended = 0;
      for (size_t p = 0; p < n.sd.size(); ++p)
      {
         const uint16_t x = g_frames[0].sd[p], y = g_frames[1].sd[p];
         for (int shift : {11, 0})
         {
            const int lo = std::min<int>((x >> shift) & 31, (y >> shift) & 31);
            const int hi = std::max<int>((x >> shift) & 31, (y >> shift) & 31);
            const int v = (n.sd[p] >> shift) & 31;
            CHECK(v >= lo && v <= hi);
         }
         blended += n.sd[p] != x && n.sd[p] != y;
         const uint8_t first = g_frames[0].zones[p];
         const uint8_t second = g_frames[1].zones[p] == 0 ? 4 : g_frames[1].zones[p];
         CHECK(n.zones[p] == (k < 1 ? first : second)); // weight 1/4 then 1/2 and 3/4
      }
      CHECK(blended > 0);
   }

   // Without a second frame the list says so, and Apply makes nothing.
   resetProject(128, 32, false);
   openMenu();
   change("target", "frame");
   CHECK(fieldOf("frames") && choiceValues(*fieldOf("frames")) == std::vector<std::string>{"0"});
   CHECK(fieldOf("frames")->help.find("Ctrl+click") != std::string::npos);
   closeForm(true);
   CHECK(g_endEditApplied == 0 && g_frames.size() == 2 && g_messages.size() == 1 &&
         g_messages[0].find("Ctrl+click") != std::string::npos);

   // Three selected frames: exactly two are needed.
   resetProject(128, 32, false);
   g_frames.push_back(makeSourceFrame(false));
   g_current = 0;
   g_selected = {0, 1, 2};
   openMenu();
   change("target", "frame");
   CHECK(fieldOf("source") && fieldOf("source")->label.find("exactly two") != std::string::npos);
   closeForm(false);

   // The window follows the selection while it is open, and the start is the frame clicked last.
   resetProject(128, 32, false);
   g_frames.push_back(makeSourceFrame(false));
   selectTwo(0, 1);
   openMenu();
   change("target", "frame");
   CHECK(fieldOf("source")->label.find("From frame 0 to frame 1") != std::string::npos);
   selectTwo(1, 0);
   fireEvent(CDMDPI_EVT_ON_SELECTION_CHANGED);
   CHECK(fieldOf("source")->label.find("From frame 1 to frame 0") != std::string::npos);
   closeForm(false);
}

// The preview draws dynamic zones in the colors of their sets (the colorized picture of a dynamic
// pixel is black); the frames Apply makes keep the picture as it is.
void testPreviewShowsDynamicContent()
{
   resetProject(128, 32, true);
   std::fill(g_frames[0].sd.begin(), g_frames[0].sd.end(), 0); // a fully dynamic frame: nothing colorized
   openMenu();
   change("transition", "wipe");
   change("option", "left");
   change("frames", "4");
   const Form::Field* pv = fieldOf("preview");
   CHECK(pv && pv->images.size() == 4);
   long lit = 0;
   for (size_t k = 0; pv && k < pv->images.size(); ++k)
      for (size_t p = 0; p < pv->images[k].pixels.size(); ++p)
      {
         const uint16_t px = pv->images[k].pixels[p];
         if (px == 0)
            continue;
         ++lit;
         CHECK(px == g_frames[0].dynamicColors[g_frames[0].zones[p] * kColors + g_frames[0].original[p]]);
      }
   CHECK(lit > 0);
   change("placement", "end");
   closeForm(true);
   CHECK(g_frames.size() == 6);
   for (size_t k = 2; k < g_frames.size(); ++k) // the new frames keep an unpainted picture
      for (uint16_t px : g_frames[k].sd)
         CHECK(px == 0);

   // A fully dynamic frame can fade: the colors its zones show count, not its black picture.
   resetProject(128, 32, true);
   std::fill(g_frames[0].sd.begin(), g_frames[0].sd.end(), 0);
   openMenu();
   change("transition", "fade");
   CHECK(fieldOf("frames") && choiceValues(*fieldOf("frames")).size() > 1);
   closeForm(false);

   // To another frame the second frame's zones use the colors of the sets as the new frames number them.
   resetProject(128, 32, true);
   g_frames[1] = makeSecondFrame(true);
   std::fill(g_frames[1].sd.begin(), g_frames[1].sd.end(), 0);
   selectTwo(0, 1);
   openMenu();
   change("target", "frame");
   change("transition", "wipe");
   change("option", "left");
   change("frames", "4");
   pv = fieldOf("preview");
   CHECK(pv && pv->images.size() == 4);
   const Frame& b = g_frames[1];
   long fromSecond = 0;
   for (size_t p = 0; pv && p < pv->images[3].pixels.size(); ++p)
      if (pv->images[3].pixels[p] != g_frames[0].sd[p])
      {
         // The first frame's own pixels are colored by its sets, the second one's by its colors.
         const uint16_t px = pv->images[3].pixels[p];
         const uint16_t own = g_frames[0].dynamicColors[g_frames[0].zones[p] * kColors + g_frames[0].original[p]];
         const uint16_t other = b.dynamicColors[b.zones[p] * kColors + b.original[p]];
         if (px != own)
            CHECK(px == other);
         fromSecond += px == other && px != own;
      }
   CHECK(fromSecond > 0);
   closeForm(false);
}

// A fade changes the colors, not the original shades: a dynamic zone colors a pixel by its shade, so
// fading the shades would step through the zone's colors. A cross-fade switches the shades over
// with the zones.
void testFadeKeepsTheOriginalShades()
{
   for (const char* target : {"black", "white"})
   {
      resetProject(128, 32, true);
      openMenu();
      change("transition", "fade");
      change("target", target);
      change("frames", "3");
      change("placement", "end");
      closeForm(true);
      CHECK(g_frames.size() == 5);
      for (size_t k = 2; k < g_frames.size(); ++k)
      {
         CHECK(g_frames[k].original == g_frames[0].original);
         CHECK(g_frames[k].zones == g_frames[0].zones);
         CHECK(g_frames[k].sd != g_frames[0].sd); // the picture does fade
      }
   }

   resetProject(128, 32, true);
   g_frames[1] = makeSecondFrame(true);
   selectTwo(0, 1);
   openMenu();
   change("target", "frame");
   change("transition", "fade");
   change("frames", "3");
   change("placement", "end");
   closeForm(true);
   CHECK(g_frames.size() == 5);
   for (size_t k = 0; k < 3 && g_frames.size() == 5; ++k)
      CHECK(g_frames[2 + k].original == (k < 1 ? g_frames[0].original : g_frames[1].original)); // weight 1/4, then 1/2, 3/4
}

// ---- Pixelate, Zoom, Wave, Checkerboard ------------------------------------------------------

std::unique_ptr<transitions::Effect> makeNamed(const char* id, const char* option, uint32_t w, uint32_t h, bool toFrame = false,
                                               bool secondMoves = false)
{
   transitions::SourceInfo info = makeInfo(w, h);
   info.toFrame = toFrame;
   info.secondMoves = secondMoves;
   return transitions::findTransition(id)->make(option, info);
}

void testPixelate()
{
   auto e = makeNamed("pixelate", "", 128, 32);
   CHECK(e->maxSteps() == 31 && !e->takesPixelsAway()); // block sizes 2 .. 32
   // The last frame: blocks of 32 x 32, every pixel of a block shows the pixel at the block's middle.
   for (uint32_t y = 0; y < 32; ++y)
      for (uint32_t x = 0; x < 128; ++x)
      {
         const transitions::Rule r = e->rule(x, y, 31, 31);
         CHECK(r.visible && !r.second && r.x == x / 32 * 32 + 16 && r.y == 16);
      }
   // The first frame has blocks of 2.
   const transitions::Rule first = e->rule(5, 7, 1, 31);
   CHECK(first.x == 5 && first.y == 7); // 2 x 2 blocks: the pixel at the middle (odd) of the block
   CHECK(e->rule(4, 6, 1, 31).x == 5 && e->rule(4, 6, 1, 31).y == 7);

   // It eases in: with 8 frames the first ones have small blocks, the growth speeds up, the last one is the largest.
   uint32_t previous = 0;
   for (uint32_t step = 1; step <= 8; ++step)
   {
      uint32_t size = 0; // the width of the first block
      for (uint32_t x = 0; x < 128; ++x)
         size += e->rule(x, 0, step, 8).x == e->rule(0, 0, step, 8).x;
      CHECK(size > previous);                              // always bigger than the frame before
      CHECK(step < 8 ? size <= 2u * (step * step * 31u / 64u + 2u) : size == 32);
      previous = size;
   }
   CHECK(e->rule(0, 0, 1, 8).x <= 2 && e->rule(0, 0, 4, 8).x <= 8); // not big by the half way

   // To another frame: twice the levels, the blocks grow on the first frame, then shrink on the other.
   auto both = makeNamed("pixelate", "", 128, 32, true);
   CHECK(both->maxSteps() == 62);
   for (uint32_t step = 1; step <= 62; ++step)
   {
      const transitions::Rule r = both->rule(10, 10, step, 62);
      CHECK(r.visible && r.second == (step > 31));
   }
   CHECK(both->rule(0, 0, 31, 62).x == 16 && both->rule(0, 0, 31, 62).y == 16); // the largest blocks of the first
   CHECK(both->rule(0, 0, 32, 62).x == 16 && both->rule(0, 0, 32, 62).y == 16); // the same, of the other
   CHECK(both->rule(0, 0, 62, 62).x == 1 && both->rule(0, 0, 62, 62).second);   // the other frame, nearly sharp
}

void testZoom()
{
   // Zoom Out: a centred picture that shrinks to 1/16 of the width, the rest is taken away.
   auto out = makeNamed("zoom", "out", 128, 32);
   CHECK(out->maxSteps() == 120 && out->takesPixelsAway());
   const uint32_t smallest = 128 - 120;
   long first = 0, last = 0;
   for (uint32_t y = 0; y < 32; ++y)
      for (uint32_t x = 0; x < 128; ++x)
      {
         first += out->rule(x, y, 1, 120).visible;
         const transitions::Rule r = out->rule(x, y, 120, 120);
         last += r.visible;
         if (r.visible)
            CHECK(x >= (128 - smallest) / 2 && x < (128 - smallest) / 2 + smallest);
      }
   CHECK(first == 127 * 32 || first == 127 * 32 - 0 || first > 127 * 31); // nearly all of it at the first step
   CHECK(last == static_cast<long>(smallest) * 2); // 8 x 2 pixels at the end
   // It shows the whole picture inside the rectangle: the corners of the rectangle are the corners.
   const transitions::Rule corner = out->rule((128 - 64) / 2, (32 - 16) / 2, 64, 120);
   CHECK(corner.visible && corner.x == 0 && corner.y == 0);

   // Zoom In: every pixel stays; the last frame shows the middle eighth of the width.
   auto in = makeNamed("zoom", "in", 128, 32);
   CHECK(in->maxSteps() == 112 && !in->takesPixelsAway());
   const transitions::Rule a = in->rule(0, 0, 112, 112), b = in->rule(127, 31, 112, 112);
   CHECK(a.visible && b.visible && a.x == 56 && b.x >= 56 + 14 && b.x < 128 - 56);
   CHECK(a.y >= 12 && b.y <= 19); // the middle of the height
   // To another frame: in on the first frame, then out of the other.
   auto both = makeNamed("zoom", "in", 128, 32, true);
   CHECK(both->maxSteps() == 224);
   CHECK(!both->rule(3, 3, 112, 224).second && both->rule(3, 3, 113, 224).second);
   CHECK(both->rule(3, 3, 112, 224).x == both->rule(3, 3, 113, 224).x);
   CHECK(both->rule(0, 0, 224, 224).second && both->rule(0, 0, 224, 224).x <= 1); // nearly full size
}

void testWave()
{
   // The wave: zero at the start and the middle of its period, the amplitude at a quarter, minus it at three quarters.
   CHECK(transitions::waveShift(0, 16, 10) == 0 && transitions::waveShift(8, 16, 10) == 0);
   CHECK(transitions::waveShift(4, 16, 10) == 10 && transitions::waveShift(12, 16, 10) == -10);
   CHECK(transitions::waveShift(16 + 4, 16, 10) == 10); // and it repeats
   for (uint32_t a = 0; a < 40; ++a)
      for (uint32_t p = 0; p < 16; ++p)
         CHECK(std::abs(transitions::waveShift(p, 16, a)) <= static_cast<int32_t>(a));

   // Horizontal: the rows slide sideways (one wave over the 32 rows), what is uncovered is taken away.
   auto h = makeNamed("wave", "horizontal", 128, 32);
   CHECK(h->maxSteps() == 127);
   // Every row moves right by the pull (20) at least; a crest row (8) by half of it more, a trough row (24) not.
   const transitions::Rule crest = h->rule(50, 8, 20, 127);
   CHECK(crest.visible && crest.x == 10 && crest.y == 8); // 50 - 20 - 10 - 10
   CHECK(!h->rule(39, 8, 20, 127).visible);                // uncovered on the left
   const transitions::Rule trough = h->rule(50, 24, 20, 127);
   CHECK(trough.visible && trough.x == 30 && trough.y == 24);
   const transitions::Rule still = h->rule(50, 16, 20, 127); // row 16: on the wave's middle
   CHECK(still.visible && still.x == 20 && still.y == 16);
   // At the end only a sliver of the trough rows is left: one column, none of the crest rows.
   long left = 0;
   for (uint32_t y = 0; y < 32; ++y)
      for (uint32_t x = 0; x < 128; ++x)
         left += h->rule(x, y, 127, 127).visible;
   CHECK(left > 0 && left <= 32);
   CHECK(h->rule(127, 24, 127, 127).visible && h->rule(127, 24, 127, 127).x == 0);
   // Vertical: the columns slide up and down (a period of 32 columns on 128), 32 steps for 32 rows.
   auto v = makeNamed("wave", "vertical", 128, 32);
   CHECK(v->maxSteps() == 31);
   const transitions::Rule down = v->rule(8, 20, 8, 31); // column 8 is a crest: moved down by 8 + 4 + 4
   CHECK(down.visible && down.x == 8 && down.y == 4);
   CHECK(!v->rule(8, 3, 8, 31).visible);
}

// ---- Melt, and the other frame that moves in with the effect ----------------------------------

void testMelt()
{
   // The drips: a speed of 0 .. 255 for every column, always the same, stretched to both ends.
   const std::vector<uint8_t> speeds = transitions::dripSpeeds(128);
   CHECK(speeds == transitions::dripSpeeds(128) && speeds.size() == 128);
   CHECK(*std::min_element(speeds.begin(), speeds.end()) == 0 && *std::max_element(speeds.begin(), speeds.end()) == 255);
   long within = 0, between = 0; // neighbours in a group of four are more alike than neighbours of two groups
   for (size_t i = 1; i < speeds.size(); ++i)
      (i % 4 ? within : between) += std::abs(static_cast<int>(speeds[i]) - static_cast<int>(speeds[i - 1]));
   CHECK(within / 3 < between / 1 && between > 0);
   CHECK(transitions::dripSpeeds(1).size() == 1 && transitions::dripSpeeds(0).empty());

   auto smooth = makeNamed("melt", "smooth", 128, 32);
   auto drips = makeNamed("melt", "drips", 128, 32);
   auto wave = makeNamed("wave", "vertical", 128, 32);
   CHECK(smooth->maxSteps() == 31 && drips->maxSteps() == 31 && wave->maxSteps() == 31);
   // Smooth melts like the vertical wave; dripping does not.
   bool smoothLikeWave = true, dripsDiffer = false;
   for (uint32_t y = 0; y < 32; ++y)
      for (uint32_t x = 0; x < 128; ++x)
      {
         const transitions::Rule a = smooth->rule(x, y, 12, 31), b = wave->rule(x, y, 12, 31), c = drips->rule(x, y, 12, 31);
         smoothLikeWave = smoothLikeWave && a.visible == b.visible && (!a.visible || (a.x == b.x && a.y == b.y));
         dripsDiffer = dripsDiffer || a.visible != c.visible || (a.visible && a.y != c.y);
      }
   CHECK(smoothLikeWave && dripsDiffer);
   // The columns only go down: a column shows the picture from further up, every pixel at least `pull` down.
   for (uint32_t x = 0; x < 128; ++x)
      for (uint32_t y = 0; y < 32; ++y)
      {
         const transitions::Rule r = drips->rule(x, y, 12, 31);
         CHECK(!r.visible || (r.x == x && r.y + 12 <= y));
      }
   // The last frame: only the slowest column(s) show a bit, one row of them.
   long left = 0;
   for (uint32_t y = 0; y < 32; ++y)
      for (uint32_t x = 0; x < 128; ++x)
         left += drips->rule(x, y, 31, 31).visible;
   CHECK(left >= 1 && left <= 8);
}

void testSecondMoves()
{
   // Only the transitions that have a pattern to follow offer it.
   for (const auto& t : transitions::kTransitions)
   {
      const std::string id = t.id;
      CHECK(t.canMoveSecond == (id == "slide" || id == "alternating_slide" || id == "wave" || id == "melt"));
   }

   // Without the option nothing changes: what the effect uncovers shows the other frame in place
   // (an invisible rule), whatever the target.
   auto still = makeNamed("slide", "left", 128, 32, true, false);
   CHECK(!still->rule(127, 5, 10, 20).visible);

   // Slide Left: the other frame follows right behind, like the next picture of a film strip.
   const uint32_t steps = 20;
   auto push = makeNamed("slide", "left", 128, 32, true, true);
   for (uint32_t step = 1; step <= steps; ++step)
   {
      const uint32_t o = transitions::offsetAt(128, step, steps);
      for (uint32_t y = 0; y < 32; ++y)
         for (uint32_t x = 0; x < 128; ++x)
         {
            const transitions::Rule r = push->rule(x, y, step, steps);
            CHECK(r.visible && r.y == y && r.x < 128);
            if (x + o < 128)
               CHECK(!r.second && r.x == x + o);
            else
               CHECK(r.second && r.x == x + o - 128); // the other frame's left edge touches the first frame's right edge
         }
   }
   // The others go the other ways: Right, Up, Down.
   auto right = makeNamed("slide", "right", 128, 32, true, true);
   const uint32_t o = transitions::offsetAt(128, 7, steps);
   CHECK(right->rule(o - 1, 0, 7, steps).second && right->rule(o - 1, 0, 7, steps).x == 127);
   CHECK(!right->rule(o, 0, 7, steps).second && right->rule(o, 0, 7, steps).x == 0);
   auto down = makeNamed("slide", "down", 128, 32, true, true);
   const uint32_t od = transitions::offsetAt(32, 7, steps);
   CHECK(down->rule(5, od - 1, 7, steps).second && down->rule(5, od - 1, 7, steps).y == 31);
   auto up = makeNamed("slide", "up", 128, 32, true, true);
   CHECK(up->rule(5, 32 - od, 7, steps).second && up->rule(5, 32 - od, 7, steps).y == 0);

   // Every effect that moves its second frame in shows a picture everywhere, always inside the
   // frame, and the first frame goes while the second takes over: the first frame's share only shrinks.
   struct Kind { const char* id; const char* option; };
   const Kind kinds[] = {{"slide", "left"},  {"slide", "right"}, {"slide", "up"},  {"slide", "down"},
                         {"alternating_slide", "horizontal"}, {"alternating_slide", "vertical"},
                         {"wave", "horizontal"}, {"wave", "vertical"}, {"melt", "smooth"}, {"melt", "drips"}};
   for (const Kind& k : kinds)
      for (uint32_t w : {128u, 129u, 192u})
      {
         const uint32_t h = w == 129u ? 33u : w == 192u ? 64u : 32u;
         auto e = makeNamed(k.id, k.option, w, h, true, true);
         const uint32_t most = e->maxSteps();
         size_t before = static_cast<size_t>(w) * h;
         for (uint32_t step = 1; step <= most; ++step)
         {
            size_t first = 0;
            for (uint32_t y = 0; y < h; ++y)
               for (uint32_t x = 0; x < w; ++x)
               {
                  const transitions::Rule r = e->rule(x, y, step, most);
                  CHECK(r.visible && r.x < w && r.y < h);
                  first += !r.second;
               }
            CHECK(first > 0 && first <= before); // some of the first frame is left at the end
            before = first;
         }
      }

   // Melt, Smooth, following: a column shows the other frame on top, ending where the first frame
   // starts, so the two pictures meet without a gap or an overlap.
   auto melt = makeNamed("melt", "drips", 128, 32, true, true);
   for (uint32_t step : {3u, 10u, 21u, 31u})
      for (uint32_t x = 0; x < 128; ++x)
      {
         uint32_t top = 0; // rows of the other frame
         while (top < 32 && melt->rule(x, top, step, 31).second)
            ++top;
         for (uint32_t y = 0; y < 32; ++y)
         {
            const transitions::Rule r = melt->rule(x, y, step, 31);
            CHECK(r.second == (y < top)); // the other frame on top, and nowhere else
            if (r.second)
               CHECK(r.y == y + 32 - top); // its bottom row meets the first frame
         }
         if (top > 0 && top < 32)
            CHECK(melt->rule(x, top, step, 31).y == 0); // the first frame starts at its top
      }
}

void testVersionAndDescriptions()
{
   resetProject(128, 32, false);
   openMenu();
   CHECK(g_form.title == std::string("TransitionFX v") + kVersion);
   CHECK(std::string(kVersion) == "1.02");
   // Every transition has a description, shown under the transition choice, with or without options.
   for (const auto& t : transitions::kTransitions)
   {
      CHECK(t.description && std::string(t.description).size() > 10);
      change("transition", t.id);
      const Form::Field* f = fieldOf("transition");
      CHECK(f && f->help == t.description);
      // None names what is left in the last frame.
      const std::string d = t.description;
      CHECK(d.find("left at the end") == std::string::npos && d.find("thin ") == std::string::npos);
   }
   CHECK(std::string(transitions::findTransition("alternating_slide")->description) ==
         "Horizontal: even rows (0,2,4) slide left and odd rows (1,3,5) slide right\n"
         "Vertical: even columns (0,2,4) slide up and odd columns (1,3,5) slide down");
   CHECK(std::string(transitions::findTransition("fade")->description) ==
         "The first frame gradually fades to black, white or the second frame.");
   closeForm(false);
}

// Dust and Morph: the pixels fly, so the generic checks of the other effects do not apply.
std::vector<size_t> litPixels(const std::vector<uint8_t>& shades, uint8_t darkest)
{
   std::vector<size_t> out;
   for (size_t i = 0; i < shades.size(); ++i)
      if (shades[i] > darkest)
         out.push_back(i);
   return out;
}

void testDust()
{
   const uint32_t w = 128, h = 32;
   const transitions::SourceInfo info = makeInfo(w, h);
   for (const char* wind : {"right", "left", "up", "down"})
   {
      auto e = makeNamed("dust", wind, w, h);
      const uint32_t most = e->maxSteps();
      CHECK(most >= 8 && most <= transitions::kMaxSteps);
      // The pixels shown are pixels of the frame and no place shows two of them; lit ones go away
      // from the wind's side, and the last frame still shows something.
      for (uint32_t step = 1; step <= most; ++step)
      {
         size_t shown = 0;
         for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
            {
               const transitions::Rule r = e->rule(x, y, step, most);
               if (!r.visible)
                  continue;
               ++shown;
               CHECK(r.x < w && r.y < h && !r.second && !r.blank);
            }
         CHECK(step < most || shown > 0);
      }
      // The same every time.
      auto again = makeNamed("dust", wind, w, h);
      CHECK(signatureOf(*e, w, h, most / 2, most) == signatureOf(*again, w, h, most / 2, most));
      // It starts on the side the wind comes from: a third of the way through, more of the lit pixels
      // nearest to that side have left than of those farthest from it.
      const uint32_t step = std::max(1u, most / 3);
      std::vector<std::pair<double, int>> lit; // (how far along the wind, has left)
      for (uint32_t y = 0; y < h; ++y)
         for (uint32_t x = 0; x < w; ++x)
         {
            if (info.shades[static_cast<size_t>(y) * w + x] <= 1)
               continue;
            const double u = std::string(wind) == "right" ? double(x) / w : std::string(wind) == "left" ? 1.0 - double(x) / w
                             : std::string(wind) == "down" ? double(y) / h : 1.0 - double(y) / h;
            const transitions::Rule r = e->rule(x, y, step, most);
            lit.push_back({u, (r.visible && r.x == x && r.y == y) ? 0 : 1});
         }
      std::sort(lit.begin(), lit.end());
      long near = 0, far = 0;
      for (size_t k = 0; k < lit.size() / 4; ++k)
      {
         near += lit[k].second;
         far += lit[lit.size() - 1 - k].second;
      }
      CHECK(near > far);
   }
   // Nothing lit, nothing to do.
   transitions::SourceInfo empty = makeInfo(w, h);
   empty.shades.assign(static_cast<size_t>(w) * h, 2);
   CHECK(transitions::findTransition("dust")->make("right", empty)->maxSteps() == 0);
   // With another frame underneath, what the wind has cleared shows it (invisible, not blank).
   auto e = makeNamed("dust", "right", w, h, true);
   bool cleared = false;
   for (uint32_t y = 0; y < h; ++y)
      for (uint32_t x = 0; x < w; ++x)
         cleared = cleared || (!e->rule(x, y, e->maxSteps(), e->maxSteps()).visible && !e->rule(x, y, e->maxSteps(), e->maxSteps()).blank);
   CHECK(cleared);
}

void testMorph()
{
   const uint32_t w = 128, h = 32;
   const transitions::SourceInfo info = makeInfo(w, h);
   const auto litA = litPixels(info.shades, 1);
   const auto litB = litPixels(info.shadesSecond, 1);
   CHECK(!litA.empty() && !litB.empty());
   // Not to another frame: nothing to do.
   CHECK(makeNamed("morph", "random", w, h, false)->maxSteps() == 0);
   for (const char* order : {"random", "right", "left", "down", "up"})
   {
      auto e = makeNamed("morph", order, w, h, true);
      const uint32_t most = e->maxSteps();
      CHECK(most >= 8);
      // The last frame is the other frame's lit pixels (taken from it), the rest black.
      std::vector<bool> lit(static_cast<size_t>(w) * h, false);
      for (size_t i : litB)
         lit[i] = true;
      for (uint32_t y = 0; y < h; ++y)
         for (uint32_t x = 0; x < w; ++x)
         {
            const transitions::Rule r = e->rule(x, y, most, most);
            const size_t i = static_cast<size_t>(y) * w + x;
            if (lit[i])
               CHECK(r.visible && r.second && r.x == x && r.y == y);
            else
               CHECK(r.visible && r.second && r.x == x && r.y == y); // the other frame's own background
         }
      // On the way, nothing else is lit than the pixels travelling, and the count of travellers
      // never exceeds what there is; the places without a pixel show the background.
      for (uint32_t step = 1; step < most; ++step)
      {
         size_t shown = 0;
         for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
            {
               const transitions::Rule r = e->rule(x, y, step, most);
               CHECK(r.visible);
               const size_t at = static_cast<size_t>(r.y) * w + r.x;
               // A traveller is a lit pixel of its frame; the rest is background.
               shown += (r.second ? info.shadesSecond[at] : info.shades[at]) > 1 ? 1 : 0;
            }
         CHECK(shown > 0 && shown <= std::max(litA.size(), litB.size()));
      }
      auto again = makeNamed("morph", order, w, h, true);
      CHECK(signatureOf(*e, w, h, most / 2, most) == signatureOf(*again, w, h, most / 2, most));
   }
   // Places without a pixel show the background, never black: where the frame has background, its own
   // pixel (the first frame's before the halfway point, the other frame's after it); where it has a
   // pixel, the background of the other frame if that has background there (the sky behind the cow
   // stays), else the nearest background pixel of the row.
   {
      auto e = makeNamed("morph", "random", w, h, true);
      const uint32_t most = e->maxSteps();
      size_t behind = 0;
      for (uint32_t step : {1u, most / 4, most / 2, most - 1})
         for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
            {
               const transitions::Rule r = e->rule(x, y, step, most);
               CHECK(r.visible); // no black
               const size_t cell = static_cast<size_t>(y) * w + x, at = static_cast<size_t>(r.y) * w + r.x;
               const bool lit = (r.second ? info.shadesSecond[at] : info.shades[at]) > 1;
               // A pixel of the picture is a traveller (somewhere else) or sits at its own place.
               if (lit)
                  continue;
               // Background: a vacated place of the first frame shows the other frame's background there.
               if (info.shades[cell] > 1 && info.shadesSecond[cell] <= 1 && r.second && r.x == x && r.y == y)
                  ++behind;
            }
      CHECK(behind > 0);
   }
   // Colors pair with colors, and what both frames share stays. Frame one: color 100 on the left, 200
   // on the right; frame two: 200 on the left, 100 on the right; in the middle a block that is the
   // same in both (color 50).
   {
      transitions::SourceInfo c = makeInfo(w, h);
      c.toFrame = true;
      const size_t cells = static_cast<size_t>(w) * h;
      c.shades.assign(cells, 1);
      c.shadesSecond.assign(cells, 1);
      c.tones.assign(cells, 0);
      c.tonesSecond.assign(cells, 0);
      c.same.assign(cells, 0);
      for (uint32_t y = 8; y < 24; ++y)
         for (uint32_t x = 0; x < w; ++x)
         {
            const size_t i = static_cast<size_t>(y) * w + x;
            if (x >= 10 && x < 50)
            {
               c.shades[i] = 2;
               c.tones[i] = 100;
               c.shadesSecond[i] = 3;
               c.tonesSecond[i] = 200;
            }
            else if (x >= 78 && x < 118)
            {
               c.shades[i] = 3;
               c.tones[i] = 200;
               c.shadesSecond[i] = 2;
               c.tonesSecond[i] = 100;
            }
            else if (x >= 60 && x < 68)
            {
               c.shades[i] = c.shadesSecond[i] = 2;
               c.tones[i] = c.tonesSecond[i] = 50;
               c.same[i] = 1;
            }
         }
      auto e = transitions::findTransition("morph")->make("random", c);
      const uint32_t most = e->maxSteps();
      CHECK(most >= 8);
      // The color of the left block travels to the right block, where the same color is.
      const uint32_t step = most / 3;
      double travelled = 0;
      size_t count = 0;
      for (uint32_t y = 0; y < h; ++y)
         for (uint32_t x = 0; x < w; ++x)
         {
            const transitions::Rule r = e->rule(x, y, step, most);
            if (r.visible && !r.second && r.x >= 10 && r.x < 50 && r.y >= 8 && r.y < 24 && r.x != x)
            {
               travelled += double(x) - r.x;
               ++count;
            }
         }
      CHECK(count > 0 && travelled / count > 10);
      // The shared block stays where it is, at every step, and nothing else takes its places at the end.
      for (uint32_t st = 1; st <= most; ++st)
      {
         size_t staying = 0;
         for (uint32_t y = 8; y < 24; ++y)
            for (uint32_t x = 60; x < 68; ++x)
            {
               const transitions::Rule r = e->rule(x, y, st, most);
               staying += (r.visible && r.x == x && r.y == y) ? 1 : 0;
            }
         CHECK(st == most ? staying == 16 * 8 : staying > 0);
      }
   }
   // Two frames that are alike (the same shape, a pixel or two apart, other shades): little moves,
   // but the colors change one after the other, so there are still many frames.
   {
      transitions::SourceInfo alike = makeInfo(w, h);
      alike.toFrame = true;
      alike.shadesSecond.assign(static_cast<size_t>(w) * h, 1);
      for (size_t i = 0; i + 1 < alike.shades.size(); ++i)
         if (alike.shades[i] > 1)
            alike.shadesSecond[i + 1] = static_cast<uint8_t>(alike.shades[i] == 3 ? 2 : 3);
      for (const char* order : {"random", "right", "up"})
         CHECK(transitions::findTransition("morph")->make(order, alike)->maxSteps() >= 24);
   }
   // Two pictures of different sizes of lit area: every lit pixel of the larger one is used.
   transitions::SourceInfo odd = makeInfo(129, 33);
   odd.toFrame = true;
   CHECK(transitions::findTransition("morph")->make("random", odd)->maxSteps() >= 4);
}

void testMorphThroughTheWindow()
{
   for (bool zones : {false, true})
   {
      resetProject(128, 32, zones);
      g_frames[1] = makeSecondFrame(zones);
      // Like a real colorization: a dark red background where the original is darkest, except that the author has
      // colored in rows that the original leaves black (green pixels in the top 8 rows of frame 0).
      for (Frame* f : {&g_frames[0], &g_frames[1]})
         for (size_t p = 0; p < f->original.size(); ++p)
            if (f->original[p] == 1)
               f->sd[p] = 0x2000;
      for (uint32_t y = 0; y < 8; ++y)
         for (uint32_t x = 0; x < 128; x += 2)
            g_frames[0].sd[static_cast<size_t>(y) * 128 + x] = 0x07E0;
      if (zones)
         for (Frame* f : {&g_frames[0], &g_frames[1]})
            std::fill(f->zones.begin(), f->zones.end(), 255);
      const Frame start = g_frames[0];
      selectTwo(0, 1);
      openMenu();
      // Without another frame the window says so.
      change("transition", "morph");
      CHECK(fieldOf("frames")->choices.find("0=None") == 0);
      change("target", "frame");
      CHECK(fieldOf("frames")->choices.find("0=None") != 0);
      change("option", "right");
      change("frames", "3");
      change("placement", "end");
      closeForm(true);
      CHECK(g_frames.size() == 5);
      if (g_frames.size() != 5)
         continue;
      const Frame& b = g_frames[1];
      const Frame& last = g_frames[4];
      for (size_t p = 0; p < last.original.size(); ++p)
      {
         if (b.original[p] > 1)
            CHECK(last.original[p] == b.original[p] && last.sd[p] == b.sd[p]);
         else
            CHECK(last.original[p] <= 1 && last.sd[p] == 0x2000); // the background stays, not black
      }
      // On the way the background stays between the pixels, and the starting frame is not changed.
      const Frame& mid = g_frames[3];
      CHECK(std::count(mid.sd.begin(), mid.sd.end(), 0x2000) > static_cast<long>(mid.sd.size() / 3));
      CHECK(std::count(mid.sd.begin(), mid.sd.end(), 0) == 0);
      CHECK(g_frames[0].original == start.original && g_frames[0].sd == start.sd);
      // The colored-in pixels that the original leaves black take part: the first new frame still
      // has green ones, and they are not left behind in the top rows of the last frame.
      auto green = [](const Frame& f, bool topOnly) {
         size_t n = 0;
         for (size_t p = 0; p < f.sd.size() && (!topOnly || p < 8 * 128); ++p)
            n += f.sd[p] == 0x07E0 ? 1 : 0;
         return n;
      };
      CHECK(green(g_frames[2], false) > 0);
      CHECK(green(last, false) == 0);
   }
}

// The window in the author's language: German and French follow the editor's language; an unknown
// language, or an editor that cannot tell, gives English.
void testLanguages()
{
   struct Language
   {
      const char* code;
      const char* transitionLabel;
      const char* black;
   };
   const Language languages[] = {{"en", "Transition", "Black"},
                                 {"de", "Übergang", "Schwarz"},
                                 {"fr", "Transition", "Noir"},
                                 {"xx", "Transition", "Black"},
                                 {"", "Transition", "Black"}};
   for (const Language& l : languages)
   {
      const std::string code = l.code;
      g_language = code;
      TransitionFXPluginUnload();
      setUpApi();
      TransitionFXPluginLoad(1, &g_bus);
      resetProject(128, 32, false);
      openMenu();
      const Form::Field* t = fieldOf("transition");
      CHECK(t && t->label == l.transitionLabel);
      const Form::Field* target = fieldOf("target");
      CHECK(target && target->choices.find(std::string("black=") + l.black) != std::string::npos);
      if (t && code == "de")
         CHECK(t->choices.find("alternating_slide=Wechselnde Verschiebung") != std::string::npos);
      if (t && code == "fr")
         CHECK(t->choices.find("dust=Poussière") != std::string::npos);
      // The description under the transition and the status bar follow.
      change("transition", "fade");
      const Form::Field* f = fieldOf("transition");
      if (f && code == "de")
         CHECK(f->help.find("überblendet") != std::string::npos);
      if (f && code == "fr")
         CHECK(f->help.find("progressivement") != std::string::npos);
      if (f && (code == "en" || code == "xx" || code.empty()))
         CHECK(f->help.find("gradually fades") != std::string::npos);
      change("frames", "3");
      closeForm(true);
      CHECK(g_status.size() == 1);
      if (!g_status.empty())
      {
         if (code == "de")
            CHECK(g_status[0].find("hinzugefügt") != std::string::npos && g_status[0].find("Rückgängig") != std::string::npos);
         else if (code == "fr")
            CHECK(g_status[0].find("ajoutée(s)") != std::string::npos);
         else
            CHECK(g_status[0].find("added 3 frame(s)") != std::string::npos);
         CHECK(g_status[0].find('%') == std::string::npos); // no placeholder is left over
      }
   }
   // An editor before version 11 has no language: English.
   g_language = "de";
   TransitionFXPluginUnload();
   setUpApi();
   g_api.version = 10;
   TransitionFXPluginLoad(1, &g_bus);
   resetProject(128, 32, false);
   openMenu();
   CHECK(fieldOf("transition") && fieldOf("transition")->label == "Transition");
   g_language = "en";
   TransitionFXPluginUnload();
   setUpApi();
   TransitionFXPluginLoad(1, &g_bus);

   // Every text of every transition has a German and a French translation.
   auto translated = [](const char* text) {
      const i18n::Entry* e = i18n::find(text);
      return e && e->german && *e->german && e->french && *e->french;
   };
   for (const transitions::Transition& tr : transitions::kTransitions)
   {
      CHECK(translated(tr.label));
      CHECK(translated(tr.description));
      CHECK(!tr.optionLabel || translated(tr.optionLabel));
      // A description is a sentence: it differs in another language.
      const i18n::Entry* d = i18n::find(tr.description);
      CHECK(d && std::string(d->german) != tr.description && std::string(d->french) != tr.description);
      for (size_t i = 0; i < tr.optionCount; ++i)
         CHECK(std::string(tr.options[i].label).find(" x ") != std::string::npos || translated(tr.options[i].label));
   }
   // And the reason that limits the frames of every transition.
   for (const Case& c : allCases())
      CHECK(translated(makeEffect(c, 128, 32)->limitReason()));
}

// Dissolve WPC: the dot pattern of the Bally/Williams games, as recorded from a real transition
// (128 x 32): every step takes one more eighth of the pixels, 7 steps.
void testWpc()
{
   const uint32_t w = 128, h = 32;
   auto e = makeNamed("dissolve", "wpc", w, h);
   CHECK(e->maxSteps() == 7);
   // The groups as recorded: row 0, the first 24 pixels; row 1 and row 3, the first 8.
   const uint32_t row0[] = {4, 1, 6, 3, 0, 5, 2, 7, 5, 2, 7, 4, 1, 6, 3, 0, 6, 3, 0, 5, 2, 7, 4, 1};
   const uint32_t row1[] = {2, 7, 4, 1, 6, 3, 0, 5};
   const uint32_t row3[] = {6, 3, 0, 5, 2, 7, 4, 1};
   for (uint32_t x = 0; x < 24; ++x)
      CHECK(transitions::wpcGroup(x, 0) == row0[x]);
   for (uint32_t x = 0; x < 8; ++x)
   {
      CHECK(transitions::wpcGroup(x, 1) == row1[x]);
      CHECK(transitions::wpcGroup(x, 3) == row3[x]);
   }
   // The pattern repeats every 4 rows and every 64 columns.
   for (uint32_t y = 0; y < h; ++y)
      for (uint32_t x = 0; x < w; ++x)
         CHECK(transitions::wpcGroup(x, y) == transitions::wpcGroup(x % 64, y % 4));
   // With 7 frames, frame n has lost the groups 0 .. n - 1: an eighth (512 pixels) more each time, and
   // the pixels that are gone are the ones the recording lights.
   for (uint32_t step = 1; step <= 7; ++step)
   {
      size_t visible = 0;
      for (uint32_t y = 0; y < h; ++y)
         for (uint32_t x = 0; x < w; ++x)
         {
            const transitions::Rule r = e->rule(x, y, step, 7);
            CHECK(r.visible == (transitions::wpcGroup(x, y) >= step));
            visible += r.visible ? 1 : 0;
         }
      CHECK(visible == w * h - 512 * step);
   }
   // With fewer frames, several groups go at once, and the last frame still keeps the last group.
   for (uint32_t steps : {1u, 2u, 3u, 5u})
   {
      CHECK(visibleCount(*e, w, h, steps, steps) == w * h / 8);
      for (uint32_t step = 1; step < steps; ++step)
         CHECK(visibleCount(*e, w, h, step, steps) > visibleCount(*e, w, h, step + 1, steps));
   }
}

void testSecondMovesWindow()
{
   // The "Second frame" field: only for another frame and for the transitions that can do it.
   resetProject(128, 32, false);
   g_frames[1] = makeSecondFrame(false);
   selectTwo(0, 1);
   openMenu();
   CHECK(fieldOf("second_motion") && !fieldOf("second_motion")->visible);
   change("target", "frame");
   change("transition", "melt");
   CHECK(fieldOf("second_motion")->visible);
   change("transition", "dissolve");
   CHECK(!fieldOf("second_motion")->visible);
   change("transition", "melt");
   change("option", "smooth");
   change("frames", "3");
   change("placement", "end");
   change("second_motion", "moves");
   closeForm(true);
   CHECK(g_frames.size() == 5);
   CHECK(g_status.size() == 1 && g_status[0].find("Melt (Smooth) to frame 1, moving in with it") != std::string::npos);
   // The other frame is on top of the new frames, in the first column: the picture's rows follow the melt.
   const Frame& a = g_frames[0];
   const Frame& b = g_frames[1];
   for (int k = 0; k < 3 && g_frames.size() == 5; ++k)
   {
      const Frame& n = g_frames[2 + k];
      long fromSecond = 0, black = 0;
      for (size_t p = 0; p < n.sd.size(); ++p)
      {
         const bool isA = n.sd[p] == a.sd[p], isB = n.sd[p] == b.sd[p];
         fromSecond += isB && !isA;
         black += n.sd[p] == 0;
      }
      CHECK(black == 0 && fromSecond > 0);
   }

   // Closed with the choice "stays in place": the other frame stays under it (the same frames as without the option).
   g_status.clear();
   selectTwo(0, 1);
   openMenu();
   change("target", "frame");
   change("transition", "melt");
   CHECK(fieldOf("second_motion")->value == "moves"); // the last choice is remembered
   change("second_motion", "still");
   change("frames", "3");
   change("placement", "end");
   closeForm(true);
   CHECK(g_status.size() == 1 && g_status[0].find("Melt (Smooth) to frame 1") != std::string::npos &&
         g_status[0].find("moving in") == std::string::npos);
}


void testCheckerboard()
{
   const transitions::Transition* t = transitions::findTransition("checkerboard");
   CHECK(t && t->optionCount == 4 && std::string(t->optionLabel) == "Square Size");
   CHECK(std::string(t->options[0].label) == "2 x 2" && std::string(t->options[3].label) == "16 x 16");
   // 16 x 16 squares on 128 x 32: 16 squares, 15 steps, one square is left.
   auto e = makeNamed("checkerboard", "16", 128, 32);
   CHECK(e->maxSteps() == 15);
   auto tileVisible = [&](uint32_t tx, uint32_t ty, uint32_t step, uint32_t steps) {
      const bool v = e->rule(tx * 16, ty * 16, step, steps).visible;
      for (uint32_t y = 0; y < 16; ++y)
         for (uint32_t x = 0; x < 16; ++x)
            CHECK(e->rule(tx * 16 + x, ty * 16 + y, step, steps).visible == v); // a square goes as a whole
      return v;
   };
   // After 8 of the 15 squares have gone, all of them are squares of one color of the board.
   int gone = 0;
   for (uint32_t ty = 0; ty < 2; ++ty)
      for (uint32_t tx = 0; tx < 8; ++tx)
         if (!tileVisible(tx, ty, 8, 15))
         {
            ++gone;
            CHECK(((tx + ty) & 1u) == 0);
         }
   CHECK(gone == 8);
   // At the end exactly one square is left, and it is one of the other color.
   int left = 0;
   for (uint32_t ty = 0; ty < 2; ++ty)
      for (uint32_t tx = 0; tx < 8; ++tx)
         if (tileVisible(tx, ty, 15, 15))
         {
            ++left;
            CHECK(((tx + ty) & 1u) == 1);
         }
   CHECK(left == 1);
   // Squares of 2 x 2 on 128 x 32: 1024 squares, 255 steps.
   CHECK(makeNamed("checkerboard", "2", 128, 32)->maxSteps() == 255);
   // An edge that is not a multiple of the square: the partial squares count.
   CHECK(makeNamed("checkerboard", "16", 129, 33)->maxSteps() == 9 * 3 - 1);
}

// Through the window to another frame: the effects that keep every pixel go through both frames.
void testGoingThroughBothFrames()
{
   for (const char* id : {"pixelate", "zoom"})
   {
      resetProject(128, 32, true);
      g_frames[1] = makeSecondFrame(true);
      selectTwo(0, 1);
      openMenu();
      change("target", "frame");
      change("transition", id);
      if (transitions::same(id, "zoom"))
         change("option", "in");
      const Form::Field* f = fieldOf("frames");
      CHECK(f && !choiceValues(*f).empty());
      change("frames", "4");
      change("placement", "end");
      closeForm(true);
      CHECK(g_frames.size() == 6);
      const Frame& a = g_frames[0];
      const Frame& b = g_frames[1];
      for (size_t k = 0; g_frames.size() == 6 && k < 4; ++k)
      {
         const Frame& n = g_frames[2 + k];
         long fromFirst = 0, fromSecond = 0;
         for (size_t p = 0; p < n.sd.size(); ++p)
         {
            // Every pixel comes from one frame or the other, with its shade and zone (the second
            // frame's zone of set 0 now in set 5).
            long matchFirst = 0, matchSecond = 0;
            for (size_t q = 0; q < a.sd.size() && !(matchFirst && matchSecond); ++q)
            {
               if (n.sd[p] == a.sd[q] && n.original[p] == a.original[q] && n.zones[p] == a.zones[q])
                  matchFirst = 1;
               if (n.sd[p] == b.sd[q] && n.original[p] == b.original[q] &&
                   n.zones[p] == (b.zones[q] == 0 ? 4 : b.zones[q]))
                  matchSecond = 1;
            }
            CHECK(matchFirst || matchSecond);
            fromFirst += matchFirst && !matchSecond;
            fromSecond += matchSecond && !matchFirst;
         }
         // The first two frames show only the first frame's pixels, the last two only the other's.
         if (k < 2)
            CHECK(fromSecond == 0);
         else
            CHECK(fromFirst == 0);
      }
   }
}

void testScene()
{
   // The new frames become a scene that the starting frame starts; the plain frames go again.
   resetProject(128, 32, true);
   mark(1, 7);
   openMenu();
   change("transition", "slide");
   change("option", "up");
   change("frames", "5");
   change("placement", "scene");
   change("duration", "40");
   closeForm(true);
   CHECK(g_endEditApplied == 1 && g_messages.empty());
   CHECK(g_scenes.size() == 1);
   if (g_scenes.size() == 1)
   {
      const Scene& scene = g_scenes[0];
      CHECK(scene.id == 50000 && scene.frames == 5 && scene.durationMs == 40);
      CHECK(scene.name == "Slide (Up) to Black" && scene.repeat == 0 && scene.finish == CDMDPI_SCENE_KEEP_LAST &&
            scene.flags == 0);
   }
   CHECK(g_frames.size() == 7);
   // The scene's frames lie right behind the starting frame; the project's other frame follows.
   CHECK(g_frames[0].startsScene == 50000 && g_frames[6].background == 7 && g_frames[6].inScene == CDMDPI_NONE);
   CHECK(g_frames[0].inScene == CDMDPI_NONE);
   for (size_t i = 1; i <= 5; ++i)
      CHECK(g_frames[i].inScene == 50000);
   CHECK(g_showCalls == 0 && statusFrames(1, 5));
   CHECK(litPixels(g_frames[5]) < litPixels(g_frames[1]));
   CHECK(g_status.size() == 1 && g_status[0].find("as scene 50000") != std::string::npos);
   CHECK(g_settings["duration"] == "40");

   // The starting frame can join the scene; it does not count as a step.
   resetProject(128, 32, true);
   mark(1, 7);
   openMenu();
   CHECK(fieldOf("include") && !fieldOf("include")->visible);
   change("transition", "slide");
   change("option", "up");
   change("frames", "5");
   change("placement", "scene");
   CHECK(fieldOf("include")->visible);
   // The WPC dissolve is always its 7 frames.
   change("transition", "dissolve");
   change("option", "wpc");
   CHECK((choiceValues(*fieldOf("frames")) == std::vector<std::string>{"7"}));
   change("transition", "slide");
   change("option", "up");
   change("frames", "5");
   // To black there is no ending frame to offer.
   CHECK((choiceValues(*fieldOf("include")) == std::vector<std::string>{"start", "none"}));
   change("include", "start");
   closeForm(true);
   CHECK(g_endEditApplied == 1 && g_messages.empty() && g_scenes.size() == 1);
   CHECK(g_scenes.size() == 1 && g_scenes[0].frames == 6);
   CHECK(g_frames.size() == 8);
   if (g_frames.size() == 8)
   {
      CHECK(g_frames[1].original == g_frames[0].original);
      CHECK(litPixels(g_frames[1]) > litPixels(g_frames[2]));
   }
   CHECK(statusFrames(1, 6));

   // Only the ending frame, to another frame: it is the other frame as it is.
   resetProject(128, 32, true);
   g_frames[1] = makeSecondFrame(true);
   selectTwo(0, 1);
   openMenu();
   change("target", "frame");
   change("transition", "wipe");
   change("option", "left");
   change("frames", "2");
   change("placement", "scene");
   CHECK((choiceValues(*fieldOf("include")) == std::vector<std::string>{"start", "end", "both", "none"}));
   change("include", "end");
   closeForm(true);
   CHECK(g_scenes.size() == 1 && g_scenes[0].frames == 3 && g_frames.size() == 5);
   if (g_frames.size() == 5)
      CHECK(g_frames[3].original == g_frames[4].original); // the last frame of the scene is the second frame

   // A refused scene takes everything back.
   resetProject(128, 32, false);
   g_refuseScene = true;
   runFlow("wipe", "left", 3, "scene");
   CHECK(g_frames.size() == 2 && g_scenes.empty() && g_endEditApplied == 0);
   CHECK(g_messages.size() == 1 && g_messages[0].find("Nothing was changed") != std::string::npos);

   // A duration that is no number is refused before anything is made.
   resetProject(128, 32, false);
   openMenu();
   change("placement", "scene");
   change("duration", "fast");
   closeForm(true);
   CHECK(g_frames.size() == 2 && g_messages.size() == 1 && g_messages[0].find("duration") != std::string::npos);

   // The duration starts as the frame's own.
   resetProject(128, 32, false);
   g_frames[0].durationMs = 80;
   openMenu();
   CHECK(fieldOf("duration") && fieldOf("duration")->value == "80");
   closeForm(false);

   // A frame that started another scene starts the new one, and the author hears of it.
   resetProject(128, 32, false);
   g_scenes.push_back({50000, "Old", 33, 0, 0, 0, 0});
   g_frames[0].startsScene = 50000;
   runFlow("dissolve", "orderly", 2, "scene");
   CHECK(g_scenes.size() == 2 && g_scenes[1].id == 50001 && g_frames[0].startsScene == 50001);
   CHECK(g_messages.size() == 1 && g_messages[0].find("scene 50000") != std::string::npos);

   // Scenes need 128 x 32: other projects are not offered them, and a saved choice falls back.
   resetProject(192, 64, false);
   g_settings["placement"] = "scene";
   openMenu();
   const Form::Field* place = fieldOf("placement");
   CHECK(place && (choiceValues(*place) == std::vector<std::string>{"after", "end"}) && place->value == "after");
   closeForm(false);
}

} // namespace

int main()
{
   setUpApi();
   TransitionFXPluginLoad(1, &g_bus);
   CHECK(g_menuCallback != nullptr);
   CHECK(g_subscriptions.size() == 2);

   testRegistry();
   testBayerIsAPermutation();
   testEveryEffectOnEverySize();
   testDissolveSpecifics();
   testMovingSpecifics();
   testFadeSpecifics();
   testPreviewSteps();

   testTheWindow();
   testTheWindowRemembers();
   testFramesListMatchesTheMaximum();
   testSavedFramesAreClamped();
   testCancelChangesNothing();
   testRefusalRollsBack();
   testTheWindowFollowsTheCurrentFrame();
   testNoFrameSelected();
   testOneWindowAtATime();
   testAllBlackFrameCannotFadeToBlack();
   testBackgroundCaveat();
   testNoRepeatedFramesInThePlugin();
   testPlacementAfterTheSource();
   testPlacementAtTheEnd();
   testRefusedMoveRollsBack();
   testScene();
   testAnotherFrame();
   testPreviewShowsDynamicContent();
   testFadeKeepsTheOriginalShades();
   testPixelate();
   testZoom();
   testWave();
   testMelt();
   testSecondMoves();
   testDust();
   testMorph();
   testMorphThroughTheWindow();
   testSecondMovesWindow();
   testWpc();
   testLanguages();
   testVersionAndDescriptions();
   testCheckerboard();
   testGoingThroughBothFrames();
   // Every transition through the window, on the sizes of real projects (single plane at the
   // ROM resolution for 192x64 and 256x64), without and with dynamic zones.
   const uint32_t sizes[][2] = {{128, 32}, {192, 64}, {256, 64}};
   for (const auto& sz : sizes)
   {
      testEveryTransitionThroughTheWindow(sz[0], sz[1], false);
      testEveryTransitionThroughTheWindow(sz[0], sz[1], true);
   }

   // Unloading closes an open window and ends the subscriptions.
   resetProject(128, 32, false);
   openMenu();
   TransitionFXPluginUnload();
   CHECK(g_menuCallback == nullptr && !g_form.open && g_subscriptions.empty());

   // An editor before version 10 gets no menu entry.
   g_api.version = 9;
   TransitionFXPluginLoad(1, &g_bus);
   CHECK(g_menuCallback == nullptr && g_subscriptions.empty());
   TransitionFXPluginUnload();

   if (g_failures)
   {
      std::printf("%d check(s) failed\n", g_failures);
      return 1;
   }
   std::printf("all tests passed\n");
   return 0;
}
