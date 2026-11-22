// license:GPLv3+
//
// TransitionFX: a plugin for the PPUC Colorizing DMD editor.
//
// Plugins menu -> "TransitionFX...". One window (a live form of the editor API version 10)
// holds everything: the transition, its option (its direction, its type ...), the number of
// frames, where the new frames go, and a preview of the transition on the author's own frame.
// The fields follow each other while the author fills them in: the options belong to the
// transition, the numbers of frames are exactly those that work for the transition on this
// frame, and the preview shows the frames Apply would make.
//
// The window does not block the editor. It follows the current frame: the author can click
// another frame while it is open, and the window starts from that one.
//
// The plugin then adds that many new frames, right after the starting frame or at the end of the
// project, each a step further along the transition than the one before; or it makes a scene of
// them that the starting frame starts. The last one is not completely black. All of it is one
// step of the editor's history (one Undo).

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "ColorizingDMDPlugin.h"
#include "Language.h"
#include "Preview.h"
#include "Transitions.h"
#include "Version.h"
#include "plugins/MsgPlugin.h"

namespace {

const MsgPluginAPI* g_msg = nullptr;
const ColorizingDMDEditorAPI* g_editor = nullptr;
uint32_t g_endpoint = 0;
unsigned int g_getApiId = 0;
unsigned int g_selectionChangedId = 0;
unsigned int g_projectChangedId = 0;

constexpr const char* kTitle = "TransitionFX";
// The window's title: the name and the version (src/Version.h).
const std::string kWindowTitle = std::string("TransitionFX v") + kVersion;
constexpr const char* kUndoName = "TransitionFX";

// The fields of the window. Their values are kept as settings when the author clicks Apply, so
// the window opens on the last choices.
constexpr const char* kFieldSource = "source"; // the note that names the starting frame
constexpr const char* kFieldTransition = "transition";
constexpr const char* kFieldOption = "option";
constexpr const char* kFieldTarget = "target"; // "black" or "white": what the transition goes to
constexpr const char* kFieldMotion = "second_motion"; // "still" or "moves": what the other frame does
constexpr const char* kFieldFrames = "frames";
// A note that draws a thin line between the groups of fields (a note starting with a tag is HTML).
constexpr const char* kSeparator = "<hr>";
// A blank line between the transition and its option; shown with the option only.
constexpr const char* kFieldGap = "gap";
constexpr const char* kGap = "<span>&nbsp;</span>";
constexpr const char* kFieldPlacement = "placement"; // "after", "end" or "scene"
constexpr const char* kFieldDuration = "duration";   // the scene's frame duration
constexpr const char* kFieldIncludeStart = "include_start"; // the scene also holds the starting frame
constexpr const char* kFieldIncludeEnd = "include_end";     // the scene also holds the ending frame
constexpr const char* kFieldPreview = "preview";

// The scenes libserum plays need ROM frames of this size.
constexpr uint32_t kSceneWidth = 128;
constexpr uint32_t kSceneHeight = 32;

// A plane of the colorized frame: its pixels, and the size of the grid they lie on.
struct Plane
{
   int hd = 0;
   uint32_t width = 0;
   uint32_t height = 0;
   std::vector<uint16_t> pixels;
};

// The frame the transition starts from.
struct Source
{
   uint32_t frame = CDMDPI_NONE;
   ColorizingDMDFrameInfo info{};
   uint32_t colorCount = 0;
   uint32_t originalWidth = 0;
   uint32_t originalHeight = 0;
   std::vector<uint8_t> original;
   std::vector<Plane> planes;
   std::vector<uint8_t> zones; // the dynamic zones, set per pixel, 255 for none
   uint32_t zonesWidth = 0;
   uint32_t zonesHeight = 0;
   std::vector<uint16_t> dynamicColors; // colors[set * colorCount + shade]
};

// The second frame of a transition to another frame, made to fit under the first: the dynamic
// zones of both are on one plane, and the dynamic sets that both use for different colors have to
// be told apart.
struct Combined
{
   std::vector<uint8_t> zones1;       // the zones of the first frame on the plane of both (255: none)
   std::vector<uint8_t> zones2;       // those of the second, its sets renamed where they clashed
   uint32_t zonesWidth = 0;
   uint32_t zonesHeight = 0;
   bool hasZones = false;             // either frame has dynamic zones
   // The dynamic sets the new frames get from the second frame: the colors of its sets as they
   // are now numbered (the first frame's own sets come along when a frame is made like it).
   std::vector<std::pair<uint32_t, std::vector<uint16_t>>> sets;
   std::string caveat;
};

// The open window: the frame it starts from (read again whenever the current frame or the
// project changes), and the effect its fields describe.
struct Window
{
   bool open = false;
   Source source;
   std::string sourceProblem; // why there is no source; empty when there is one
   bool scenesPossible = false;
   const transitions::Transition* transition = nullptr;
   const transitions::Option* option = nullptr; // null for a transition without options
   std::unique_ptr<transitions::Effect> effect;
   bool toWhite = false; // the transition goes to white instead of black
   // The transition goes to another frame: the frame you clicked last is the start, the other
   // selected frame the end. `second` is then the end frame and `combined` how it fits under
   // the first one.
   bool toFrame = false;
   Source second;
   std::string secondProblem; // why there is no second frame; empty when there is one
   Combined combined;
   // The other frame comes in with the effect, in its pattern, instead of staying in place (only
   // for the transitions that can do that).
   bool secondMoves = false;
   // The number of frames the author asked for: the list shows it whenever the transition can
   // make that many, and the most it can make otherwise.
   std::string framesWanted;
};
Window g_window;

void message(const char* title, const std::string& text)
{
   if (g_editor)
      g_editor->ShowMessage(g_endpoint, title, text.c_str());
}

// Reads a buffer the editor fills: the first call (capacity 0) asks for the size.
template <class T, class Call>
bool fetch(Call call, std::vector<T>& out, uint32_t& width, uint32_t& height)
{
   T probe{};
   width = 0;
   height = 0;
   const uint32_t count = call(&probe, 0u, &width, &height);
   out.clear();
   if (count == 0)
      return false;
   out.assign(count, T{});
   if (call(out.data(), count, &width, &height) != count)
   {
      out.clear();
      return false;
   }
   return true;
}

std::string setting(const char* key, const char* fallback)
{
   char buffer[128];
   const uint32_t size = g_editor->GetSetting(g_endpoint, key, buffer, sizeof(buffer));
   if (size == 0 || size > sizeof(buffer))
      return fallback;
   return buffer;
}

void saveSetting(const char* key, const std::string& value)
{
   g_editor->SetSetting(g_endpoint, key, value.c_str());
}

// What a field of the open window shows now.
std::string formValue(const char* key, const char* fallback)
{
   char buffer[128];
   const uint32_t size = g_editor->GetFormValue(g_endpoint, key, buffer, sizeof(buffer));
   if (size == 0 || size > sizeof(buffer))
      return fallback;
   return buffer;
}

// The option a transition was last used with is kept per transition.
std::string optionKey(const transitions::Transition& transition) { return std::string("option_") + transition.id; }

// What the author chose, in words: "Wipe (Spiral) to White".
std::string describe(const transitions::Transition& transition, const transitions::Option* option,
                     const std::string& target)
{
   std::string what = i18n::tr(transition.label);
   if (option)
      what += std::string(" (") + i18n::tr(option->label) + ")";
   return i18n::fmt("%1 to %2", {what, target});
}

// What the window's "Transition to" says now: "Black", "White" or "frame 12".
std::string targetName()
{
   if (g_window.toFrame)
      return i18n::fmt("frame %1", {std::to_string(g_window.second.frame)}) +
             (g_window.secondMoves && g_window.transition && g_window.transition->canMoveSecond
                 ? i18n::tr(", moving in with it")
                 : "");
   return i18n::tr(g_window.toWhite ? "White" : "Black");
}

// ---- Reading the frame ----------------------------------------------------------------------

// Reads the frame the transition starts from. Returns an empty string when it worked.
std::string loadSource(uint32_t frame, Source& source)
{
   source = Source{};
   ColorizingDMDProjectInfo info{};
   if (!g_editor->GetProjectInfo(&info) || !info.hasProject)
      return i18n::tr("Open a project first.");
   if (frame == CDMDPI_NONE || frame >= info.frameCount)
      return i18n::tr("Select the frame to start the transition from first.");

   source.frame = frame;
   source.colorCount = info.colorCount;
   if (!g_editor->GetFrameInfo(frame, &source.info))
      return i18n::tr("The frame does not exist.");

   if (!fetch<uint8_t>(
          [&](uint8_t* p, uint32_t cap, uint32_t* w, uint32_t* h) {
             return g_editor->GetOriginalFrame(frame, p, cap, w, h);
          },
          source.original, source.originalWidth, source.originalHeight))
      return i18n::tr("The editor could not give the original frame.");

   for (int hd = 0; hd <= 1; ++hd)
   {
      if (hd == 1 && !source.info.hasHd)
         continue;
      Plane plane;
      plane.hd = hd;
      if (fetch<uint16_t>(
             [&](uint16_t* p, uint32_t cap, uint32_t* w, uint32_t* h) {
                return g_editor->GetColorizedFrame(frame, hd, p, cap, w, h);
             },
             plane.pixels, plane.width, plane.height))
         source.planes.push_back(std::move(plane));
   }
   if (source.planes.empty())
      return i18n::tr("The editor could not give the colorized frame.");

   if (source.info.hasDynamicPixels)
   {
      fetch<uint8_t>(
         [&](uint8_t* p, uint32_t cap, uint32_t* w, uint32_t* h) {
            return g_editor->GetDynamicMask(frame, p, cap, w, h);
         },
         source.zones, source.zonesWidth, source.zonesHeight);
      uint16_t probe = 0;
      const uint32_t count = g_editor->GetDynamicColors(frame, &probe, 0);
      if (count > 0)
      {
         source.dynamicColors.assign(count, 0);
         if (g_editor->GetDynamicColors(frame, source.dynamicColors.data(), count) != count)
            source.dynamicColors.clear();
      }
   }
   return std::string();
}

// What the effects need to know about the frame.
// The largest difference between two colors of the same pixel of the two frames, in units of a
// 5-bit channel (green has 6 bits, so it counts half).
uint32_t crossSpanOf(const Source& a, const Source& b)
{
   uint32_t span = 0;
   for (size_t i = 0; i < a.planes.size() && i < b.planes.size(); ++i)
   {
      const std::vector<uint16_t>& x = a.planes[i].pixels;
      const std::vector<uint16_t>& y = b.planes[i].pixels;
      for (size_t k = 0; k < x.size() && k < y.size(); ++k)
      {
         if (x[k] == y[k])
            continue;
         auto diff = [](int u, int v) { return static_cast<uint32_t>(u > v ? u - v : v - u); };
         const uint32_t dr = diff((x[k] >> 11) & 31, (y[k] >> 11) & 31);
         const uint32_t dg = (diff((x[k] >> 5) & 63, (y[k] >> 5) & 63) + 1u) / 2u;
         const uint32_t db = diff(x[k] & 31, y[k] & 31);
         span = std::max({span, dr, dg, db});
      }
   }
   return span;
}

// The shades Dust and Morph move: the original frame's, and also the places where the original is
// as dark as it gets but the colorized frame shows something other than its background (the
// color most pixels have): an author can color in rows the original leaves black, and those
// pixels move too, while a colored background stays where it is. `background` is set to a place
// that shows the background (UINT32_MAX: none, every place has something on it).
std::vector<uint8_t> movingShades(const Source& source, uint32_t& background,
                                  const std::set<uint16_t>& backgroundColors = {})
{
   background = UINT32_MAX;
   std::vector<uint8_t> shades = source.original;
   const uint32_t w = source.originalWidth, h = source.originalHeight;
   if (shades.empty() || shades.size() != static_cast<size_t>(w) * h)
      return shades;
   const uint8_t darkest = *std::min_element(shades.begin(), shades.end());
   if (darkest == 255)
      return shades;
   const Plane* plane = source.planes.empty() ? nullptr : &source.planes[0];
   if (plane && (plane->width == 0 || plane->height == 0 ||
                 plane->pixels.size() != static_cast<size_t>(plane->width) * plane->height))
      plane = nullptr;
   uint16_t backgroundColor = 0;
   if (plane)
   {
      std::vector<uint32_t> count(65536, 0);
      for (uint16_t pixel : plane->pixels)
         ++count[pixel];
      backgroundColor = static_cast<uint16_t>(std::max_element(count.begin(), count.end()) - count.begin());
   }
   const bool zones = source.zonesWidth > 0 && source.zonesHeight > 0 &&
                      source.zones.size() == static_cast<size_t>(source.zonesWidth) * source.zonesHeight;
   for (uint32_t y = 0; y < h; ++y)
      for (uint32_t x = 0; x < w; ++x)
      {
         uint8_t& shade = shades[static_cast<size_t>(y) * w + x];
         if (shade > darkest)
            continue;
         bool shown = false;
         if (plane)
         {
            const size_t px = static_cast<size_t>(x) * plane->width / w;
            const size_t py = static_cast<size_t>(y) * plane->height / h;
            const uint16_t color = plane->pixels[py * plane->width + px];
            shown = color != backgroundColor && !backgroundColors.count(color);
         }
         if (!shown && zones)
         {
            const size_t zx = static_cast<size_t>(x) * source.zonesWidth / w;
            const size_t zy = static_cast<size_t>(y) * source.zonesHeight / h;
            shown = source.zones[zy * source.zonesWidth + zx] != 255;
         }
         if (shown)
            shade = static_cast<uint8_t>(darkest + 1);
         else if (background == UINT32_MAX)
            background = y * w + x;
      }
   return shades;
}

// The color of every place of the original frame, as the colorized picture shows it (one value
// each, for comparing); empty when there is no colorized picture.
std::vector<uint16_t> tonesOf(const Source& source)
{
   std::vector<uint16_t> tones;
   const uint32_t w = source.originalWidth, h = source.originalHeight;
   if (source.planes.empty() || source.original.size() != static_cast<size_t>(w) * h)
      return tones;
   const Plane& plane = source.planes[0];
   if (plane.width == 0 || plane.height == 0 || plane.pixels.size() != static_cast<size_t>(plane.width) * plane.height)
      return tones;
   tones.resize(static_cast<size_t>(w) * h);
   for (uint32_t y = 0; y < h; ++y)
      for (uint32_t x = 0; x < w; ++x)
      {
         const size_t px = static_cast<size_t>(x) * plane.width / w, py = static_cast<size_t>(y) * plane.height / h;
         tones[static_cast<size_t>(y) * w + x] = plane.pixels[py * plane.width + px];
      }
   return tones;
}

// The places that look the same in both frames: the same shade and the same color (and the same
// dynamic zone). Empty when the frames cannot be compared place by place.
std::vector<uint8_t> sameOf(const Source& a, const Source& b, const std::vector<uint16_t>& tonesA,
                            const std::vector<uint16_t>& tonesB)
{
   const size_t cells = static_cast<size_t>(a.originalWidth) * a.originalHeight;
   if (a.originalWidth != b.originalWidth || a.originalHeight != b.originalHeight || a.original.size() != cells ||
       b.original.size() != cells || tonesA.size() != tonesB.size())
      return {};
   std::vector<uint8_t> same(cells, 0);
   for (size_t i = 0; i < cells; ++i)
      same[i] = a.original[i] == b.original[i] && (tonesA.empty() || tonesA[i] == tonesB[i]) ? 1 : 0;
   return same;
}

transitions::SourceInfo sourceInfoOf(const Source& source, bool toWhite, const Source* second = nullptr)
{
   transitions::SourceInfo info;
   info.toWhite = toWhite;
   if (second)
   {
      info.toFrame = true;
      info.crossSpan = crossSpanOf(source, *second);
   }
   info.width = source.originalWidth;
   info.height = source.originalHeight;
   info.colorCount = source.colorCount;
   info.tones = tonesOf(source);
   if (second)
   {
      info.tonesSecond = tonesOf(*second);
      info.same = sameOf(source, *second, info.tones, info.tonesSecond);
      // The colors of the places that are the same in both frames and dark in the original are the
      // background (a sky with dithering has several): they are not pixels to move elsewhere.
      std::set<uint16_t> background;
      if (info.same.size() == info.tones.size() && info.tones.size() == source.original.size() && !source.original.empty())
      {
         const uint8_t darkest = *std::min_element(source.original.begin(), source.original.end());
         // Only colors with a good share of the frame: a few black pixels are not a background.
         std::map<uint16_t, size_t> count;
         for (size_t i = 0; i < info.same.size(); ++i)
            if (info.same[i] && source.original[i] <= darkest)
               ++count[info.tones[i]];
         for (const auto& c : count)
            if (c.second * 50 >= info.same.size())
               background.insert(c.first);
      }
      info.shades = movingShades(source, info.background, background);
      info.shadesSecond = movingShades(*second, info.backgroundSecond, background);
   }
   else
      info.shades = movingShades(source, info.background);
   if (!source.original.empty())
   {
      info.brightestShade = *std::max_element(source.original.begin(), source.original.end());
      info.darkestShade = *std::min_element(source.original.begin(), source.original.end());
   }
   std::vector<bool> seen(65536, false);
   for (const Plane& plane : source.planes)
      for (uint16_t pixel : plane.pixels)
         if (!seen[pixel])
         {
            seen[pixel] = true;
            info.colors.push_back(pixel);
         }
   // The colors the dynamic zones show are colors of the frame too (its colorized picture is
   // black where a zone colors the pixel).
   if (!source.zones.empty() && source.colorCount > 0)
   {
      bool used[CDMDPI_DYNAMIC_SETS] = {};
      for (uint8_t set : source.zones)
         if (set < CDMDPI_DYNAMIC_SETS)
            used[set] = true;
      for (uint32_t set = 0; set < CDMDPI_DYNAMIC_SETS; ++set)
         for (uint32_t shade = 0; used[set] && shade < source.colorCount; ++shade)
         {
            const size_t index = static_cast<size_t>(set) * source.colorCount + shade;
            if (index < source.dynamicColors.size() && !seen[source.dynamicColors[index]])
            {
               seen[source.dynamicColors[index]] = true;
               info.colors.push_back(source.dynamicColors[index]);
            }
         }
   }
   return info;
}

// Puts the second frame under the first: the zones of both on one plane, and where both use the
// same dynamic set for different colors, the second frame's zones moved to a set nobody uses, with
// that set's colors, so the first frame's set gradually gives way to the second frame's.
void combine(const Source& a, const Source& b, Combined& out)
{
   out = Combined{};
   if (a.zones.empty() && b.zones.empty())
      return;
   out.hasZones = true;
   out.zonesWidth = a.zones.empty() ? b.zonesWidth : a.zonesWidth;
   out.zonesHeight = a.zones.empty() ? b.zonesHeight : a.zonesHeight;
   const size_t size = static_cast<size_t>(out.zonesWidth) * out.zonesHeight;
   out.zones1 = a.zones.empty() ? std::vector<uint8_t>(size, 255) : a.zones;
   out.zones2 = b.zones.empty() ? std::vector<uint8_t>(size, 255) : b.zones;

   bool used1[CDMDPI_DYNAMIC_SETS] = {};
   bool used2[CDMDPI_DYNAMIC_SETS] = {};
   for (uint8_t set : out.zones1)
      if (set < CDMDPI_DYNAMIC_SETS)
         used1[set] = true;
   for (uint8_t set : out.zones2)
      if (set < CDMDPI_DYNAMIC_SETS)
         used2[set] = true;

   const uint32_t count = a.colorCount;
   auto colorsOf = [&](const Source& source, uint32_t set) {
      std::vector<uint16_t> colors;
      if (count > 0 && static_cast<size_t>(set + 1) * count <= source.dynamicColors.size())
         colors.assign(source.dynamicColors.begin() + static_cast<size_t>(set) * count,
                       source.dynamicColors.begin() + static_cast<size_t>(set + 1) * count);
      return colors;
   };

   // Where the second frame's sets go: its own number unless the first frame uses that set for
   // other colors.
   uint32_t target[CDMDPI_DYNAMIC_SETS];
   bool taken[CDMDPI_DYNAMIC_SETS] = {};
   for (uint32_t set = 0; set < CDMDPI_DYNAMIC_SETS; ++set)
      target[set] = set;
   std::vector<uint32_t> clashing;
   for (uint32_t set = 0; set < CDMDPI_DYNAMIC_SETS; ++set)
   {
      if (!used2[set])
         continue;
      if (used1[set] && colorsOf(a, set) != colorsOf(b, set))
         clashing.push_back(set);
      else if (!used1[set])
         out.sets.emplace_back(set, colorsOf(b, set)); // its own number is free: just bring the colors
   }
   std::string lost;
   for (uint32_t set : clashing)
   {
      uint32_t free = CDMDPI_NONE;
      for (uint32_t f = 0; f < CDMDPI_DYNAMIC_SETS && free == CDMDPI_NONE; ++f)
         if (!used1[f] && !used2[f] && !taken[f])
            free = f;
      if (free == CDMDPI_NONE)
      {
         target[set] = 255; // no set left: the zone cannot come along
         lost += (lost.empty() ? "" : ", ") + std::to_string(set + 1);
         continue;
      }
      taken[free] = true;
      target[set] = free;
      out.sets.emplace_back(free, colorsOf(b, set));
      out.caveat += i18n::fmt("Both frames use dynamic set %1 for different colors: frame %2's zones of that set use "
                              "dynamic set %3 in the new frames.",
                              {std::to_string(set + 1), std::to_string(b.frame), std::to_string(free + 1)}) +
                    "\n";
   }
   if (!lost.empty())
      out.caveat += i18n::fmt("No dynamic set was free for frame %1's set(s) %2: those zones are not in the new frames.",
                              {std::to_string(b.frame), lost}) +
                    "\n";
   for (uint8_t& set : out.zones2)
      if (set < CDMDPI_DYNAMIC_SETS)
         set = static_cast<uint8_t>(target[set]);
}

// The other selected frame: the frame you clicked last is the start, the other frame of exactly
// two selected frames the end. Returns the reason there is none, or an empty string.
std::string chooseSecond(uint32_t current, uint32_t& second)
{
   second = CDMDPI_NONE;
   std::vector<uint32_t> selected;
   uint32_t probe = 0;
   const uint32_t total = g_editor->GetSelectedFrames(&probe, 0);
   if (total > 0 && total < 100000)
   {
      selected.assign(total, 0);
      selected.resize(std::min(total, g_editor->GetSelectedFrames(selected.data(), total)));
   }
   std::vector<uint32_t> others;
   for (uint32_t frame : selected)
      if (frame != current)
         others.push_back(frame);
   if (others.empty())
      return i18n::tr("Ctrl+click a second frame in the frame list: the frame you clicked last is the start, the "
                      "other selected frame the end.");
   if (others.size() > 1)
      return i18n::tr("Select exactly two frames: the frame you clicked last is the start, the other one the end.");
   second = others.front();
   return std::string();
}

// ---- Making the frames ----------------------------------------------------------------------

// What Apply makes.
struct Request
{
   uint32_t steps = 0;
   std::string placement; // "after", "end" or "scene"
   uint32_t sceneDurationMs = 0;
   bool includeStart = false; // scenes only: the starting frame, unchanged, as the scene's first frame
   bool includeEnd = false;   // scenes only: the ending frame (the other frame, black or white), as its last
};

// What Apply made, for the status bar.
struct Result
{
   std::vector<uint32_t> shown; // the new frames (of the scene)
   uint32_t sceneId = CDMDPI_NONE;
   std::string caveat;
};

// One colorized plane of a step: the source's, with the second frame underneath when the
// transition goes to another frame, else the black or white fill.
void drawPlane(const Plane& plane, const Plane* under, uint16_t fill, const transitions::Effect& effect,
               uint32_t step, uint32_t steps, std::vector<uint16_t>& out)
{
   const Source& source = g_window.source;
   if (under && effect.crossFades())
      preview::renderCross<uint16_t>(plane.pixels, under->pixels, out, effect, step, steps, preview::mixColor);
   else
      preview::renderStep<uint16_t>(plane.pixels, out, plane.width, plane.height, source.originalWidth,
                                    source.originalHeight, effect, step, steps, fill,
                                    [&](uint16_t color) { return effect.color(color, step, steps); },
                                    under ? &under->pixels : nullptr);
}

// Creates the transition from the window's source frame and effect. Returns an empty string
// when it worked, else what went wrong (and nothing was changed).
std::string createTransition(const Request& request, Result& result)
{
   const Source& source = g_window.source;
   const transitions::Effect& effect = *g_window.effect;
   const uint32_t steps = request.steps;
   const bool scene = request.placement == "scene";

   // The window offers only numbers that work; this is the net under it.
   if (steps < 1 || steps > effect.maxSteps())
      return i18n::fmt("This transition can make at most %1 different frames from this frame.",
                       {std::to_string(effect.maxSteps())});
   if (scene && !g_window.scenesPossible)
      return i18n::tr("Scenes need a project of 128 x 32 frames.");
   const bool toFrame = g_window.toFrame;
   if (toFrame && !g_window.secondProblem.empty())
      return g_window.secondProblem;
   const Source& second = g_window.second;
   const Combined& combined = g_window.combined;

   if (source.info.background != CDMDPI_NONE)
      result.caveat += std::string(i18n::tr("The frame shows a background. Where the background shows through, the "
                                            "pixels stay as they are; only the frame's own pixels are changed by the "
                                            "transition.")) +
                       "\n";
   if (toFrame && second.info.background != CDMDPI_NONE)
      result.caveat += i18n::fmt("Frame %1 shows a background; the new frames keep the background of frame %2.",
                                 {std::to_string(second.frame), std::to_string(source.frame)}) +
                       "\n";
   if (toFrame)
      result.caveat += combined.caveat;
   if (scene && source.info.triggerId >= 50000 && source.info.triggerId <= 61999)
      result.caveat += i18n::fmt("Frame %1 started scene %2 before; it now starts the new scene.",
                                 {std::to_string(source.frame), std::to_string(source.info.triggerId)}) +
                       "\n";

   if (!g_editor->BeginEdit(g_endpoint, kUndoName))
      return i18n::tr("The editor cannot start an edit right now (another edit is open).");

   auto fail = [&](const std::string& why) {
      g_editor->EndEdit(g_endpoint, 0); // takes every change back
      return why + " " + i18n::tr("Nothing was changed.");
   };

   const uint32_t ow = source.originalWidth, oh = source.originalHeight;
   std::vector<uint8_t> stepOriginal;
   std::vector<uint16_t> stepPixels;
   std::vector<uint8_t> stepZones;
   std::vector<uint16_t> stepColors(source.colorCount);
   std::vector<uint32_t> created;
   // What a pixel the transition has taken away becomes: black (shade 0, RGB565 0) or white (the
   // brightest shade, RGB565 0xFFFF). Such a pixel is in no dynamic zone.
   const uint8_t fillShade = g_window.toWhite ? static_cast<uint8_t>(source.colorCount - 1u) : static_cast<uint8_t>(0);
   const uint16_t fillColor = g_window.toWhite ? static_cast<uint16_t>(0xFFFF) : static_cast<uint16_t>(0);

   // A frame of the scene that is not a step of the effect: the starting frame as it is, or the end
   // the transition reaches (the other frame as it is, or all black, or all white).
   auto addEdge = [&](bool atEnd) -> std::string {
      const bool other = atEnd && toFrame;
      const bool filled = atEnd && !toFrame;
      const Source& from = other ? second : source;
      const std::string which = i18n::tr(atEnd ? "the ending frame" : "the starting frame");
      if (filled)
         stepOriginal.assign(source.original.size(), fillShade);
      const std::vector<uint8_t>& original = filled ? stepOriginal : from.original;
      uint32_t newFrame = CDMDPI_NONE;
      if (!g_editor->AddFrame(g_endpoint, original.data(), static_cast<uint32_t>(original.size()), from.frame,
                              &newFrame) ||
          newFrame == CDMDPI_NONE)
         return i18n::fmt("The editor refused to add %1.", {which});
      created.push_back(newFrame);
      if (!g_editor->SetFrameComparison(g_endpoint, newFrame, CDMDPI_NONE, from.info.shapeMode))
         return i18n::fmt("The editor refused to clear the comparison mask of %1.", {which});
      for (size_t index = 0; index < source.planes.size(); ++index)
      {
         const Plane& plane = other ? second.planes[index] : source.planes[index];
         const std::vector<uint16_t>* pixels = &plane.pixels;
         if (filled)
         {
            stepPixels.assign(plane.pixels.size(), fillColor);
            pixels = &stepPixels;
         }
         if (!g_editor->SetFramePicture(g_endpoint, newFrame, plane.hd, pixels->data(), nullptr,
                                        static_cast<uint32_t>(pixels->size())))
            return i18n::fmt("The editor refused the picture of %1.", {which});
      }
      if (filled)
      {
         // All one shade: no pixel is in a dynamic zone, and no rotation keeps turning colors.
         if (!source.zones.empty())
         {
            stepZones.assign(source.zones.size(), 255);
            if (!g_editor->SetDynamicZones(g_endpoint, newFrame, stepZones.data(),
                                           static_cast<uint32_t>(stepZones.size())))
               return i18n::fmt("The editor refused the dynamic zones of %1.", {which});
         }
         if (!g_editor->ClearColorRotations(g_endpoint, newFrame))
            return i18n::fmt("The editor refused to clear the color rotations of %1.", {which});
      }
      return std::string();
   };

   if (scene && request.includeStart)
   {
      const std::string why = addEdge(false);
      if (!why.empty())
         return fail(why);
   }

   for (uint32_t step = 1; step <= steps; ++step)
   {
      const std::string which = i18n::fmt("frame %1 of %2", {std::to_string(step), std::to_string(steps)});

      // The original: the pixels taken away get the shade of black (0) or white (the brightest),
      // or the pixel of the other frame. A fade does not change the shades (a dynamic zone colors a pixel
      // by its shade, so fading them would step through the zone's colors, besides fading those); a
      // cross-fade switches from one frame's shades to the other's at the halfway point, with the zones.
      if (toFrame && effect.crossFades())
         preview::renderCross<uint8_t>(source.original, second.original, stepOriginal, effect, step, steps,
                                       preview::mixZone);
      else
         preview::renderStep<uint8_t>(source.original, stepOriginal, ow, oh, ow, oh, effect, step, steps, fillShade,
                                      [&](uint8_t shade) { return effect.shade(shade, step, steps); },
                                      toFrame ? &second.original : nullptr);

      // The new frame is made like the source: its settings and pictures come along.
      uint32_t newFrame = CDMDPI_NONE;
      if (!g_editor->AddFrame(g_endpoint, stepOriginal.data(), static_cast<uint32_t>(stepOriginal.size()),
                              source.frame, &newFrame) ||
          newFrame == CDMDPI_NONE)
         return fail(i18n::fmt("The editor refused to add %1.", {which}));
      created.push_back(newFrame);

      // No comparison mask on the new frames (the source's mask came along with the copy). The
      // shape mode stays as it is.
      if (!g_editor->SetFrameComparison(g_endpoint, newFrame, CDMDPI_NONE, source.info.shapeMode))
         return fail(i18n::fmt("The editor refused to clear the comparison mask of %1.", {which}));

      // The colorized planes follow the same decisions as the original.
      for (size_t index = 0; index < source.planes.size(); ++index)
      {
         const Plane& plane = source.planes[index];
         drawPlane(plane, toFrame ? &second.planes[index] : nullptr, fillColor, effect, step, steps, stepPixels);
         if (!g_editor->SetFramePicture(g_endpoint, newFrame, plane.hd, stepPixels.data(), nullptr,
                                        static_cast<uint32_t>(stepPixels.size())))
            return fail(i18n::fmt("The editor refused the picture of %1.", {which}));
      }

      // Dynamic zones color a pixel at run time from its shade, so they move with the picture,
      // and a pixel that was taken away must not stay in a zone.
      if (toFrame ? combined.hasZones : !source.zones.empty())
      {
         const std::vector<uint8_t>& zones = toFrame ? combined.zones1 : source.zones;
         const uint32_t zw = toFrame ? combined.zonesWidth : source.zonesWidth;
         const uint32_t zh = toFrame ? combined.zonesHeight : source.zonesHeight;
         if (toFrame && effect.crossFades())
            preview::renderCross<uint8_t>(zones, combined.zones2, stepZones, effect, step, steps, preview::mixZone);
         else
            preview::renderStep<uint8_t>(zones, stepZones, zw, zh, ow, oh, effect, step, steps, 255,
                                         [](uint8_t set) { return set; }, toFrame ? &combined.zones2 : nullptr);
         if (!g_editor->SetDynamicZones(g_endpoint, newFrame, stepZones.data(), static_cast<uint32_t>(stepZones.size())))
            return fail(i18n::fmt("The editor refused the dynamic zones of %1.", {which}));
      }
      if (toFrame)
      {
         // The sets the second frame's zones use, with their colors; and no color rotation, which
         // would keep turning the colors of the first frame over the second one's pixels.
         for (const auto& entry : combined.sets)
            if (entry.second.size() == source.colorCount &&
                !g_editor->SetDynamicColors(g_endpoint, newFrame, entry.first, entry.second.data(), source.colorCount))
               return fail(i18n::fmt("The editor refused the dynamic colors of %1.", {which}));
         if (!g_editor->ClearColorRotations(g_endpoint, newFrame))
            return fail(i18n::fmt("The editor refused to clear the color rotations of %1.", {which}));
      }

      if (effect.changesColors())
      {
         // The colors a dynamic zone shows are the frame's dynamic colors, so they fade too.
         if (!source.dynamicColors.empty() && source.colorCount > 0)
            for (uint32_t set = 0; set < source.dynamicColors.size() / source.colorCount; ++set)
            {
               for (uint32_t shade = 0; shade < source.colorCount; ++shade)
                  stepColors[shade] =
                     effect.color(source.dynamicColors[static_cast<size_t>(set) * source.colorCount + shade], step, steps);
               if (!g_editor->SetDynamicColors(g_endpoint, newFrame, set, stepColors.data(), source.colorCount))
                  return fail(i18n::fmt("The editor refused the dynamic colors of %1.", {which}));
            }
         // A color rotation would keep turning the frame's colors at full brightness.
         if (!g_editor->ClearColorRotations(g_endpoint, newFrame))
            return fail(i18n::fmt("The editor refused to clear the color rotations of %1.", {which}));
      }
   }

   if (scene && request.includeEnd)
   {
      const std::string why = addEdge(true);
      if (!why.empty())
         return fail(why);
   }

   if (scene)
   {
      // A scene of copies of the new frames, which the starting frame starts. The plain frames
      // were only the material: they go again. The scene's frames lie behind them, so they move
      // down by as many frames.
      ColorizingDMDScene definition{};
      const std::string name = describe(*g_window.transition, g_window.option, targetName());
      definition.sceneId = CDMDPI_NONE;
      definition.name = name.c_str();
      definition.groups = 1;
      definition.durationMs = request.sceneDurationMs;
      definition.repeat = 0;
      definition.finish = CDMDPI_SCENE_KEEP_LAST;
      uint32_t sceneId = CDMDPI_NONE, first = CDMDPI_NONE;
      if (!g_editor->CreateScene(g_endpoint, &definition, created.data(), static_cast<uint32_t>(created.size()),
                                 &sceneId, &first) ||
          sceneId == CDMDPI_NONE || first == CDMDPI_NONE)
         return fail(i18n::tr("The editor refused to make the scene."));
      if (!g_editor->SetFrameScene(g_endpoint, source.frame, sceneId))
         return fail(i18n::fmt("The editor refused to let frame %1 start the scene.", {std::to_string(source.frame)}));
      if (!g_editor->DeleteFrames(g_endpoint, created.data(), static_cast<uint32_t>(created.size())))
         return fail(i18n::tr("The editor refused to delete the frames the scene was made from."));
      first -= static_cast<uint32_t>(created.size());
      for (uint32_t i = 0; i < created.size(); ++i)
         created[i] = first + i;
      result.sceneId = sceneId;
      // The scene's frames lie behind the last frame of the project; move them to just behind
      // the starting frame, as the plain frames go (nothing to do when they already are there).
      if (created.front() != source.frame + 1)
      {
         if (!g_editor->MoveFrames(g_endpoint, created.data(), static_cast<uint32_t>(created.size()), source.frame + 1))
            return fail(i18n::tr("The editor refused to move the scene's frames behind the starting frame."));
         for (uint32_t i = 0; i < created.size(); ++i)
            created[i] = source.frame + 1 + i;
      }
   }
   // Right after the starting frame: the new frames were appended, so move them as one block to
   // just behind it (nothing to do when they already are there).
   else if (request.placement == "after" && created.front() != source.frame + 1)
   {
      if (!g_editor->MoveFrames(g_endpoint, created.data(), static_cast<uint32_t>(created.size()), source.frame + 1))
         return fail(i18n::tr("The editor refused to move the new frames behind the starting frame."));
      for (uint32_t i = 0; i < created.size(); ++i)
         created[i] = source.frame + 1 + i; // where they are now, in their order
   }

   if (!g_editor->EndEdit(g_endpoint, 1))
      return i18n::tr("The editor did not record the new frames. In a team project a partner is probably working on "
                      "the frame order or on this frame: the status bar names who, for a few seconds. Merge the "
                      "partners' changes first, or ask them. Nothing was changed.");

   // The current frame and the selection stay as they are (no ShowFrames): the editor's Undo
   // takes the new frames back, and the author is where they were before.
   result.shown = created;
   return std::string();
}

// ---- The window -----------------------------------------------------------------------------

// Changes one field of the open window.
void update(const char* key, const ColorizingDMDFormFieldUpdate& change)
{
   g_editor->SetFormField(g_endpoint, key, &change);
}

ColorizingDMDFormFieldUpdate changeOf(uint32_t changes)
{
   ColorizingDMDFormFieldUpdate change{};
   change.changes = changes;
   return change;
}

std::string optionChoices(const transitions::Transition& transition)
{
   std::string choices;
   for (size_t i = 0; i < transition.optionCount; ++i)
   {
      if (i > 0)
         choices += '\n';
      choices += std::string(transition.options[i].id) + "=" + i18n::tr(transition.options[i].label);
   }
   return choices;
}

std::string placementChoices(bool scenes)
{
   std::string choices = std::string("after=") + i18n::tr("Immediately after the starting frame") + "\nend=" +
                         i18n::tr("At the end of the project");
   if (scenes)
      choices += std::string("\nscene=") + i18n::tr("As a scene that the starting frame starts");
   return choices;
}

// The effect of the transition and option the window shows, on the source frame.
void makeEffect()
{
   g_window.effect.reset();
   const bool ready = g_window.sourceProblem.empty() && (!g_window.toFrame || g_window.secondProblem.empty());
   if (ready && g_window.transition)
   {
      transitions::SourceInfo info =
         sourceInfoOf(g_window.source, g_window.toWhite, g_window.toFrame ? &g_window.second : nullptr);
      info.secondMoves = g_window.secondMoves && g_window.transition->canMoveSecond;
      g_window.effect = g_window.transition->make(g_window.option ? g_window.option->id : "", info);
   }
}

// Reads the second frame (the other selected frame) and fits it under the first, when the window
// goes to another frame.
void loadSecond()
{
   g_window.second = Source{};
   g_window.combined = Combined{};
   g_window.secondProblem.clear();
   if (!g_window.toFrame || !g_window.sourceProblem.empty())
      return;
   uint32_t frame = CDMDPI_NONE;
   g_window.secondProblem = chooseSecond(g_window.source.frame, frame);
   if (!g_window.secondProblem.empty())
      return;
   g_window.secondProblem = loadSource(frame, g_window.second);
   if (!g_window.secondProblem.empty())
      return;
   const Source& a = g_window.source;
   const Source& b = g_window.second;
   bool same = a.originalWidth == b.originalWidth && a.originalHeight == b.originalHeight &&
               a.planes.size() == b.planes.size();
   for (size_t i = 0; same && i < a.planes.size(); ++i)
      same = a.planes[i].hd == b.planes[i].hd && a.planes[i].width == b.planes[i].width &&
             a.planes[i].height == b.planes[i].height && a.planes[i].pixels.size() == b.planes[i].pixels.size();
   if (same && !a.zones.empty() && !b.zones.empty())
      same = a.zonesWidth == b.zonesWidth && a.zonesHeight == b.zonesHeight;
   if (!same)
   {
      g_window.secondProblem = i18n::fmt("Frame %1 is not made like frame %2 (another size, or only one of them has "
                                         "an HD picture).",
                                         {std::to_string(b.frame), std::to_string(a.frame)});
      return;
   }
   combine(a, b, g_window.combined);
}

uint32_t mostFrames() { return g_window.effect ? g_window.effect->maxSteps() : 0; }

// The frames list: exactly 1 .. the most this transition makes from this frame. A list cannot
// be empty: when the transition can do nothing here it holds one entry that says so.
std::string framesChoices()
{
   const uint32_t most = mostFrames();
   if (g_window.toFrame && !g_window.secondProblem.empty())
      return std::string("0=") + i18n::tr("None (select the second frame first)");
   if (most < 1)
      return std::string("0=") + i18n::tr("None (this transition can do nothing with this frame)");
   std::string choices;
   for (uint32_t n = 1; n <= most; ++n)
   {
      if (n > 1)
         choices += '\n';
      choices += std::to_string(n);
   }
   return choices;
}

std::string framesHelp()
{
   if (g_window.toFrame && !g_window.secondProblem.empty())
      return g_window.secondProblem;
   if (!g_window.effect)
      return std::string();
   if (mostFrames() < 1)
      return i18n::fmt("This transition can do nothing with this frame (%1). For a fade this means the picture is "
                       "already all black, or all white%2",
                       {i18n::tr(g_window.effect->limitReason()),
                        i18n::tr(g_window.toFrame ? ", or the two frames hardly differ." : ".")});
   return i18n::fmt("Up to %1 different frames are possible from this frame (%2).",
                    {std::to_string(mostFrames()), i18n::tr(g_window.effect->limitReason())});
}

// The number of frames to open the list on: the one asked for, within what works here.
std::string framesValue(const std::string& wanted)
{
   const uint32_t most = mostFrames();
   if (most < 1)
      return "0";
   const long number = std::strtol(wanted.c_str(), nullptr, 10);
   return std::to_string(std::clamp<long>(number, 1, most));
}

std::string sourceNote()
{
   if (!g_window.sourceProblem.empty())
      return g_window.sourceProblem;
   if (g_window.toFrame)
      return g_window.secondProblem.empty()
                ? i18n::fmt("From frame %1 to frame %2. The frame you clicked last is the start, the other selected "
                            "frame the end.",
                            {std::to_string(g_window.source.frame), std::to_string(g_window.second.frame)})
                : i18n::fmt("Starting from frame %1.", {std::to_string(g_window.source.frame)}) + " " +
                     g_window.secondProblem;
   return i18n::fmt("Starting from frame %1. Click another frame in the frame list to start from that one.",
                    {std::to_string(g_window.source.frame)});
}

// The preview: a few steps of the transition, drawn from the source frame's first colorized
// plane as Apply would draw them.
struct PreviewPictures
{
   std::vector<std::vector<uint16_t>> pixels;
   std::vector<std::string> captions;
   std::vector<ColorizingDMDFormImage> images;
};

// The picture as the preview shows it: where the frame has dynamic zones, the pixels of a zone are
// drawn in the color their dynamic set gives their shade (at run time the editor's colorized
// picture has nothing there, so a fully dynamic frame would be black). Only the preview does
// this; the new frames keep their zones.
Plane staticPlane(const Source& source, const Plane& plane, const std::vector<uint8_t>& zones, uint32_t zonesWidth,
                  uint32_t zonesHeight, const std::vector<uint16_t>& colors)
{
   Plane out = plane;
   const uint32_t count = source.colorCount;
   if (zones.empty() || colors.empty() || count == 0 || zonesWidth == 0 || zonesHeight == 0 || plane.width == 0 ||
       plane.height == 0 || source.originalWidth == 0 || source.originalHeight == 0)
      return out;
   for (uint32_t y = 0; y < plane.height; ++y)
      for (uint32_t x = 0; x < plane.width; ++x)
      {
         const size_t zoneIndex = static_cast<size_t>(static_cast<uint64_t>(y) * zonesHeight / plane.height) * zonesWidth +
                                  static_cast<uint64_t>(x) * zonesWidth / plane.width;
         const size_t shadeIndex = static_cast<size_t>(static_cast<uint64_t>(y) * source.originalHeight / plane.height) *
                                      source.originalWidth +
                                   static_cast<uint64_t>(x) * source.originalWidth / plane.width;
         if (zoneIndex >= zones.size() || shadeIndex >= source.original.size())
            continue;
         const uint32_t set = zones[zoneIndex];
         const uint32_t shade = source.original[shadeIndex];
         if (set >= CDMDPI_DYNAMIC_SETS || shade >= count || static_cast<size_t>(set) * count + shade >= colors.size())
            continue;
         out.pixels[static_cast<size_t>(y) * plane.width + x] = colors[static_cast<size_t>(set) * count + shade];
      }
   return out;
}

PreviewPictures makePreview(uint32_t steps)
{
   PreviewPictures out;
   if (!g_window.effect || steps < 1 || steps > mostFrames() || g_window.source.planes.empty())
      return out;
   const Source& first = g_window.source;
   const Combined& combined = g_window.combined;
   const bool both = g_window.toFrame && combined.hasZones;
   // The second frame's sets as the new frames number them: its colors for the sets it brought along.
   std::vector<uint16_t> colors2 = first.dynamicColors;
   if (g_window.toFrame)
   {
      if (colors2.empty())
         colors2.assign(static_cast<size_t>(CDMDPI_DYNAMIC_SETS) * first.colorCount, 0);
      for (const auto& entry : combined.sets)
         if (entry.second.size() == first.colorCount &&
             (static_cast<size_t>(entry.first) + 1) * first.colorCount <= colors2.size())
            std::copy(entry.second.begin(), entry.second.end(),
                      colors2.begin() + static_cast<size_t>(entry.first) * first.colorCount);
   }
   const Plane plane = staticPlane(first, first.planes.front(), both ? combined.zones1 : first.zones,
                                   both ? combined.zonesWidth : first.zonesWidth,
                                   both ? combined.zonesHeight : first.zonesHeight, first.dynamicColors);
   Plane secondPlane;
   if (g_window.toFrame)
      secondPlane = staticPlane(g_window.second, g_window.second.planes.front(),
                                both ? combined.zones2 : g_window.second.zones,
                                both ? combined.zonesWidth : g_window.second.zonesWidth,
                                both ? combined.zonesHeight : g_window.second.zonesHeight, colors2);
   const transitions::Effect& effect = *g_window.effect;
   const uint16_t fillColor = g_window.toWhite ? static_cast<uint16_t>(0xFFFF) : static_cast<uint16_t>(0);
   const std::vector<uint32_t> shown = preview::previewSteps(steps);
   out.pixels.resize(shown.size());
   out.captions.resize(shown.size());
   for (size_t i = 0; i < shown.size(); ++i)
   {
      const uint32_t step = shown[i];
      drawPlane(plane, g_window.toFrame ? &secondPlane : nullptr, fillColor, effect, step, steps,
                out.pixels[i]);
      out.captions[i] = i18n::fmt("Frame %1 of %2", {std::to_string(step), std::to_string(steps)});
   }
   // The images point into the pixels and captions, which are complete now.
   for (size_t i = 0; i < shown.size(); ++i)
      out.images.push_back({out.pixels[i].data(), plane.width, plane.height, preview::previewScale(plane.width),
                            out.captions[i].c_str()});
   return out;
}

void updatePreview()
{
   const uint32_t steps = static_cast<uint32_t>(std::strtol(formValue(kFieldFrames, "0").c_str(), nullptr, 10));
   const PreviewPictures pictures = makePreview(steps);
   ColorizingDMDFormFieldUpdate change = changeOf(CDMDPI_FORM_SET_IMAGES | CDMDPI_FORM_SET_VISIBLE);
   change.visible = pictures.images.empty() ? 0 : 1;
   change.images = pictures.images.data();
   change.imageCount = static_cast<uint32_t>(pictures.images.size());
   update(kFieldPreview, change);
}

// The frames list for the effect the window shows now, keeping the number chosen when it still
// works; then the preview.
void updateFrames()
{
   const std::string& wanted = g_window.framesWanted;
   const std::string choices = framesChoices();
   const std::string help = framesHelp();
   const std::string value = framesValue(wanted);
   ColorizingDMDFormFieldUpdate change = changeOf(CDMDPI_FORM_SET_CHOICES | CDMDPI_FORM_SET_VALUE | CDMDPI_FORM_SET_HELP);
   change.choices = choices.c_str();
   change.value = value.c_str();
   change.help = help.c_str();
   update(kFieldFrames, change);
   updatePreview();
}

// The description of a transition: the help under the transition choice, so that every
// transition has one, with or without options.
void showDescription(const transitions::Transition& transition)
{
   ColorizingDMDFormFieldUpdate change = changeOf(CDMDPI_FORM_SET_HELP);
   change.help = i18n::tr(transition.description);
   update(kFieldTransition, change);
}

// The option field for the transition the window shows: its own label and list, the option it
// was used with last; hidden for a transition without options.
void updateOption()
{
   const transitions::Transition& transition = *g_window.transition;
   if (transition.optionCount == 0)
   {
      g_window.option = nullptr;
      ColorizingDMDFormFieldUpdate change = changeOf(CDMDPI_FORM_SET_VISIBLE);
      change.visible = 0;
      update(kFieldOption, change);
      update(kFieldGap, change);
      showDescription(transition);
      return;
   }
   g_window.option = transitions::findOption(transition, setting(optionKey(transition).c_str(), "").c_str());
   const std::string choices = optionChoices(transition);
   ColorizingDMDFormFieldUpdate change =
      changeOf(CDMDPI_FORM_SET_VISIBLE | CDMDPI_FORM_SET_LABEL | CDMDPI_FORM_SET_CHOICES | CDMDPI_FORM_SET_VALUE);
   change.visible = 1;
   change.label = i18n::tr(transition.optionLabel);
   change.choices = choices.c_str();
   change.value = g_window.option->id;
   update(kFieldOption, change);
   showDescription(transition);
   ColorizingDMDFormFieldUpdate gap = changeOf(CDMDPI_FORM_SET_VISIBLE);
   gap.visible = 1;
   update(kFieldGap, gap);
}

// The "Second frame" field: shown for a transition to another frame, by the transitions that can
// bring that frame in with the effect.
void updateMotion()
{
   ColorizingDMDFormFieldUpdate change = changeOf(CDMDPI_FORM_SET_VISIBLE);
   change.visible = g_window.toFrame && g_window.transition && g_window.transition->canMoveSecond ? 1 : 0;
   update(kFieldMotion, change);
}

void updateDuration()
{
   ColorizingDMDFormFieldUpdate change = changeOf(CDMDPI_FORM_SET_VISIBLE);
   change.visible = formValue(kFieldPlacement, "after") == "scene" ? 1 : 0;
   update(kFieldDuration, change);
   update(kFieldIncludeStart, change);
   update(kFieldIncludeEnd, change);
}

// The current frame or the project changed: the window starts from the current frame now.
void reloadSource()
{
   g_window.sourceProblem = loadSource(g_editor->GetCurrentFrame(), g_window.source);
   loadSecond();
   ColorizingDMDProjectInfo info{};
   g_window.scenesPossible = g_editor->GetProjectInfo(&info) && info.hasProject &&
                             info.originalWidth == kSceneWidth && info.originalHeight == kSceneHeight;
   makeEffect();
}

void refreshWindow()
{
   reloadSource();
   const std::string note = sourceNote();
   ColorizingDMDFormFieldUpdate change = changeOf(CDMDPI_FORM_SET_LABEL);
   change.label = note.c_str();
   update(kFieldSource, change);
   updateFrames();
}

// The author changed a field.
void MSGPIAPI onFieldChanged(void* /*userData*/, const char* key)
{
   if (!g_editor || !g_window.open)
      return;
   const std::string field = key ? key : "";
   if (field == kFieldTransition)
   {
      const transitions::Transition* transition =
         transitions::findTransition(formValue(kFieldTransition, "").c_str());
      if (!transition)
         return;
      g_window.transition = transition;
      updateOption();
      updateMotion();
      makeEffect();
      updateFrames();
   }
   else if (field == kFieldOption && g_window.transition)
   {
      g_window.option = transitions::findOption(*g_window.transition, formValue(kFieldOption, "").c_str());
      if (g_window.option)
         saveSetting(optionKey(*g_window.transition).c_str(), g_window.option->id);
      makeEffect();
      updateFrames();
   }
   else if (field == kFieldMotion)
   {
      g_window.secondMoves = formValue(kFieldMotion, "still") == "moves";
      saveSetting(kFieldMotion, g_window.secondMoves ? "moves" : "still");
      makeEffect();
      updateFrames();
   }
   else if (field == kFieldTarget)
   {
      g_window.toWhite = formValue(kFieldTarget, "black") == "white";
      g_window.toFrame = formValue(kFieldTarget, "black") == "frame";
      updateMotion();
      refreshWindow(); // another frame needs the second frame read, and the note names it
   }
   else if (field == kFieldFrames)
   {
      g_window.framesWanted = formValue(kFieldFrames, "10");
      updatePreview();
   }
   else if (field == kFieldPlacement)
      updateDuration();
}

// The window was closed: with Apply, make the frames.
void MSGPIAPI onWindowClosed(void* /*userData*/, uint32_t result)
{
   g_window.open = false;
   if (result != CDMDPI_FORM_ACCEPTED || !g_editor)
      return;

   // The project may have changed since the window last looked: start from what is there now.
   g_window.toFrame = formValue(kFieldTarget, "black") == "frame";
   g_window.toWhite = formValue(kFieldTarget, "black") == "white";
   g_window.secondMoves = formValue(kFieldMotion, "still") == "moves";
   reloadSource();
   if (!g_window.sourceProblem.empty())
   {
      message(kTitle, g_window.sourceProblem);
      return;
   }
   if (g_window.toFrame && !g_window.secondProblem.empty())
   {
      message(kTitle, g_window.secondProblem);
      return;
   }
   g_window.transition = transitions::findTransition(formValue(kFieldTransition, "").c_str());
   if (!g_window.transition)
   {
      message(kTitle, i18n::tr("Please choose a transition from the list."));
      return;
   }
   g_window.option = transitions::findOption(*g_window.transition, formValue(kFieldOption, "").c_str());
   if (g_window.option)
      saveSetting(optionKey(*g_window.transition).c_str(), g_window.option->id);
   makeEffect();

   const std::string what = describe(*g_window.transition, g_window.option, targetName());
   if (mostFrames() < 1)
   {
      message(kTitle, i18n::fmt("%1 can do nothing with this frame (%2). For a fade this means the picture is "
                                "already all black, or all white.",
                                {what, i18n::tr(g_window.effect->limitReason())}));
      return;
   }

   Request request;
   const std::string framesText = formValue(kFieldFrames, "");
   char* end = nullptr;
   const long frames = std::strtol(framesText.c_str(), &end, 10);
   if (framesText.empty() || *end != '\0' || frames < 1)
   {
      message(kTitle, i18n::tr("Please choose the number of frames from the list."));
      return;
   }
   request.steps = static_cast<uint32_t>(frames);
   request.placement = formValue(kFieldPlacement, "after");
   if (request.placement == "scene")
   {
      const std::string durationText = formValue(kFieldDuration, "");
      const long duration = std::strtol(durationText.c_str(), &end, 10);
      if (durationText.empty() || *end != '\0' || duration < 1 || duration > 65535)
      {
         message(kTitle, i18n::tr("The frame duration of the scene is a number of milliseconds from 1 to 65535."));
         return;
      }
      request.sceneDurationMs = static_cast<uint32_t>(duration);
      request.includeStart = formValue(kFieldIncludeStart, "0") == "1";
      request.includeEnd = formValue(kFieldIncludeEnd, "0") == "1";
      saveSetting(kFieldIncludeStart, request.includeStart ? "1" : "0");
      saveSetting(kFieldIncludeEnd, request.includeEnd ? "1" : "0");
   }

   Result made;
   const std::string problem = createTransition(request, made);
   if (!problem.empty())
   {
      message(kTitle, problem);
      return;
   }

   const std::string first = std::to_string(made.shown.front()), last = std::to_string(made.shown.back());
   std::string status;
   if (made.sceneId != CDMDPI_NONE)
      status = i18n::fmt("TransitionFX: added %1 frame(s) of %2 as scene %3 (frames %4 to %5), started by frame %6. "
                         "Undo takes them back.",
                         {std::to_string(frames), what, std::to_string(made.sceneId), first, last,
                          std::to_string(g_window.source.frame)});
   else
      status = i18n::fmt("TransitionFX: added %1 frame(s) of %2, as frames %3 to %4. Undo takes them back.",
                         {std::to_string(frames), what, first, last});
   g_editor->ShowStatus(g_endpoint, status.c_str());
   if (!made.caveat.empty())
      message(kTitle, made.caveat);
}

// The menu entry: opens the window on the current frame and the last choices.
void MSGPIAPI onMenuEntry(void* /*userData*/)
{
   if (!g_editor || g_window.open)
      return;

   g_window.toFrame = setting(kFieldTarget, "black") == "frame";
   reloadSource();
   if (!g_window.sourceProblem.empty())
   {
      message(kTitle, g_window.sourceProblem);
      return;
   }

   g_window.transition = transitions::findTransition(setting(kFieldTransition, "").c_str());
   if (!g_window.transition)
      g_window.transition = &transitions::kTransitions[0];
   const transitions::Transition& transition = *g_window.transition;
   g_window.option =
      transition.optionCount ? transitions::findOption(transition, setting(optionKey(transition).c_str(), "").c_str())
                             : nullptr;
   g_window.toWhite = setting(kFieldTarget, "black") == "white";
   g_window.secondMoves = setting(kFieldMotion, "still") == "moves";
   makeEffect();

   std::string transitionChoices;
   for (const auto& entry : transitions::kTransitions)
   {
      if (!transitionChoices.empty())
         transitionChoices += '\n';
      transitionChoices += std::string(entry.id) + "=" + i18n::tr(entry.label);
   }
   const std::string note = sourceNote();
   const std::string options = transition.optionCount ? optionChoices(transition) : "-";
   const std::string optionValue = g_window.option ? g_window.option->id : "-";
   const std::string frames = framesChoices();
   g_window.framesWanted = setting(kFieldFrames, "10");
   const std::string framesStart = framesValue(g_window.framesWanted);
   const std::string help = framesHelp();
   std::string placement = setting(kFieldPlacement, "after");
   if (placement != "end" && !(placement == "scene" && g_window.scenesPossible))
      placement = "after";
   const std::string placements = placementChoices(g_window.scenesPossible);
   std::string duration = setting(kFieldDuration, "");
   if (duration.empty())
      duration = std::to_string(g_window.source.info.durationMs ? g_window.source.info.durationMs : 33);
   const std::string includeStart = setting(kFieldIncludeStart, "0") == "1" ? "1" : "0";
   const std::string includeEnd = setting(kFieldIncludeEnd, "0") == "1" ? "1" : "0";
   const PreviewPictures pictures = makePreview(static_cast<uint32_t>(std::strtol(framesStart.c_str(), nullptr, 10)));

   const std::string targets = std::string("black=") + i18n::tr("Black") + "\nwhite=" + i18n::tr("White") +
                               "\nframe=" + i18n::tr("Another frame");
   const std::string motions = std::string("still=") + i18n::tr("Stays in place, revealed") + "\nmoves=" +
                               i18n::tr("Moves in with the effect");
   const ColorizingDMDFormField fields[] = {
      {kFieldSource, note.c_str(), CDMDPI_FIELD_NOTE, nullptr, nullptr, nullptr, 0, nullptr, 0},
      {kFieldTransition, i18n::tr("Transition"), CDMDPI_FIELD_RADIO, transition.id, transitionChoices.c_str(),
       i18n::tr(transition.description), 0, nullptr, 0},
      {kFieldGap, kGap, CDMDPI_FIELD_NOTE, nullptr, nullptr, nullptr, transition.optionCount ? 0u : CDMDPI_FORM_HIDDEN,
       nullptr, 0},
      {kFieldOption, i18n::tr(transition.optionLabel ? transition.optionLabel : "Option"), CDMDPI_FIELD_RADIO,
       optionValue.c_str(), options.c_str(), nullptr, transition.optionCount ? 0u : CDMDPI_FORM_HIDDEN,
       nullptr, 0},
      {"sep1", kSeparator, CDMDPI_FIELD_NOTE, nullptr, nullptr, nullptr, 0, nullptr, 0},
      {kFieldTarget, i18n::tr("Transition to"), CDMDPI_FIELD_RADIO,
       g_window.toFrame ? "frame" : (g_window.toWhite ? "white" : "black"), targets.c_str(), nullptr, 0, nullptr, 0},
      {kFieldMotion, i18n::tr("Second frame"), CDMDPI_FIELD_RADIO, g_window.secondMoves ? "moves" : "still",
       motions.c_str(),
       i18n::tr("Stays in place: the second frame is not moved, the effect uncovers it. Moves in with the effect: the "
                "second frame follows the first one in, in the same pattern (a slide pushes it in, a melt lets it "
                "drip in from the top ...)."),
       g_window.toFrame && transition.canMoveSecond ? 0u : CDMDPI_FORM_HIDDEN, nullptr, 0},
      {"sep2", kSeparator, CDMDPI_FIELD_NOTE, nullptr, nullptr, nullptr, 0, nullptr, 0},
      {kFieldFrames, i18n::tr("Number of frames"), CDMDPI_FIELD_CHOICE, framesStart.c_str(), frames.c_str(), help.c_str(), 0,
       nullptr, 0},
      {"sep3", kSeparator, CDMDPI_FIELD_NOTE, nullptr, nullptr, nullptr, 0, nullptr, 0},
      {kFieldPlacement, i18n::tr("Place the new frames"), CDMDPI_FIELD_RADIO, placement.c_str(), placements.c_str(),
       i18n::tr("A scene is made of the new frames, and the starting frame starts it. Scenes need a project of "
                "128 x 32 frames."),
       0, nullptr, 0},
      {kFieldDuration, i18n::tr("Frame duration of the scene (ms)"), CDMDPI_FIELD_TEXT, duration.c_str(), nullptr,
       i18n::tr("How long each frame of the scene shows. The other settings of the scene are the editor's defaults; "
                "change them in the Scenes section."),
       placement == "scene" ? 0u : CDMDPI_FORM_HIDDEN, nullptr, 0},
      {kFieldIncludeStart, i18n::tr("Include the starting frame in the scene"), CDMDPI_FIELD_CHECKBOX,
       includeStart.c_str(), nullptr,
       i18n::tr("Adds the starting frame, unchanged, as the first frame of the scene. Handy to set its timing. It "
                "does not count towards the number of frames above."),
       placement == "scene" ? 0u : CDMDPI_FORM_HIDDEN, nullptr, 0},
      {kFieldIncludeEnd, i18n::tr("Include the ending frame in the scene"), CDMDPI_FIELD_CHECKBOX, includeEnd.c_str(),
       nullptr,
       i18n::tr("Adds the ending frame (the other frame, or all black or all white) as the last frame of the scene. "
                "Handy to set its timing. It does not count towards the number of frames above."),
       placement == "scene" ? 0u : CDMDPI_FORM_HIDDEN, nullptr, 0},
      {"sep4", kSeparator, CDMDPI_FIELD_NOTE, nullptr, nullptr, nullptr, 0, nullptr, 0},
      {kFieldPreview, i18n::tr("Preview"), CDMDPI_FIELD_IMAGE, nullptr, nullptr, nullptr,
       pictures.images.empty() ? CDMDPI_FORM_HIDDEN : 0u, pictures.images.data(),
       static_cast<uint32_t>(pictures.images.size())},
   };
   if (!g_editor->ShowLiveForm(g_endpoint, kWindowTitle.c_str(), i18n::tr("Apply"), nullptr, 0, fields,
                               static_cast<uint32_t>(sizeof(fields) / sizeof(fields[0])), onFieldChanged,
                               onWindowClosed, nullptr))
   {
      message(kTitle, i18n::tr("The window could not be opened. Close other plugin windows and try again."));
      return;
   }
   g_window.open = true;
}

// The editor's events: while the window is open, it follows the current frame and the project.
void MSGPIAPI onEditorEvent(const unsigned int /*msgId*/, void* /*context*/, void* /*msgData*/)
{
   if (g_editor && g_window.open)
      refreshWindow();
}

} // namespace

MSGPI_EXPORT void MSGPIAPI TransitionFXPluginLoad(const uint32_t endpointId, const MsgPluginAPI* api)
{
   g_msg = api;
   g_endpoint = endpointId;

   // Ask the editor for its plugin API (see the header of ColorizingDMDPlugin.h).
   const ColorizingDMDEditorAPI* editor = nullptr;
   g_getApiId = g_msg->GetMsgID(CDMDPI_NAMESPACE, CDMDPI_MSG_GET_API);
   g_msg->BroadcastMsg(g_endpoint, g_getApiId, &editor);
   if (!editor || editor->version < 10)
   {
      // Not hosted by the PPUC Colorizing DMD editor, or by one too old for this plugin: it
      // needs the live forms and scenes of version 9 and the radio buttons of version 10.
      g_editor = nullptr;
      return;
   }
   g_editor = editor;
   // The language of the editor's menus (editor API version 11); English with an older editor.
   char language[16] = "";
   if (editor->version >= 11 && editor->GetLanguage && editor->GetLanguage(language, sizeof(language)) > 0)
      i18n::setLanguage(language);
   else
      i18n::setLanguage("en");
   g_selectionChangedId = g_msg->GetMsgID(CDMDPI_NAMESPACE, CDMDPI_EVT_ON_SELECTION_CHANGED);
   g_projectChangedId = g_msg->GetMsgID(CDMDPI_NAMESPACE, CDMDPI_EVT_ON_PROJECT_CHANGED);
   g_msg->SubscribeMsg(g_endpoint, g_selectionChangedId, onEditorEvent, nullptr);
   g_msg->SubscribeMsg(g_endpoint, g_projectChangedId, onEditorEvent, nullptr);
   g_editor->AddMenuEntry(g_endpoint, "TransitionFX...", onMenuEntry, nullptr);
}

MSGPI_EXPORT void MSGPIAPI TransitionFXPluginUnload()
{
   if (g_editor)
   {
      if (g_window.open)
         g_editor->CloseForm(g_endpoint);
      g_editor->RemoveMenuEntries(g_endpoint);
      g_msg->UnsubscribeMsg(g_selectionChangedId, onEditorEvent, nullptr);
      g_msg->UnsubscribeMsg(g_projectChangedId, onEditorEvent, nullptr);
      g_msg->ReleaseMsgID(g_selectionChangedId);
      g_msg->ReleaseMsgID(g_projectChangedId);
   }
   if (g_msg)
      g_msg->ReleaseMsgID(g_getApiId);
   g_window = Window{};
   g_editor = nullptr;
   g_msg = nullptr;
}
