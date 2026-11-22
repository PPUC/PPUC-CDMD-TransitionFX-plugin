// license:GPLv3+
//
// The transitions of the TransitionFX plugin.
//
// A transition is an Effect. For every pixel of the new frame it answers "where does this pixel
// come from?": a pixel of the starting frame, or black. The plugin asks that on the original
// frame's grid and applies the answer to the original frame, to every colorized plane (SD and
// HD) and to the dynamic zones, so an effect does not need to know anything about the editor. A
// dissolve keeps each pixel where it is or blackens it; a slide moves pixels; a split moves the
// two halves apart. Only the fade changes colors: it also gives the new color of a color.
//
// To add a transition: write an Effect, a function that makes it, and one entry in kTransitions
// (with its options, if it has any). It then shows up in the plugin's windows.
//
// Steps run from 1 to `steps`. The starting frame is step 0 and is not created. The last step
// is never completely black (and a fade to white never completely white): something of the
// picture is left. Every frame of a transition differs from the others and from the starting
// frame, so each effect has a most frames (maxSteps) for the frame it is made for.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <memory>
#include <map>
#include <numeric>
#include <optional>
#include <string_view>
#include <tuple>
#include <vector>

namespace transitions {

// The most frames any transition may create.
inline constexpr uint32_t kMaxSteps = 255;

struct Rule
{
   bool visible;         // false: the pixel is black, x and y mean nothing
   uint32_t x;           // the pixel of the starting frame it shows
   uint32_t y;
   bool second = false;  // the pixel is taken from the other frame (x and y are then a place in that
                         // one), in a transition to another frame that goes through both pictures
   bool blank = false;   // invisible only: the pixel is black even when another frame is underneath
                         // (the pixel migration shows nothing but the pixels on their way)
};

// What an effect needs to know about the frame it starts from.
struct SourceInfo
{
   uint32_t width = 0; // the original frame's size
   uint32_t height = 0;
   uint32_t colorCount = 4;       // shades of the original frame
   uint32_t brightestShade = 0;   // the brightest and the darkest shade the original uses
   uint32_t darkestShade = 0;
   std::vector<uint16_t> colors;  // the distinct RGB565 colors of the colorized pictures
   bool toWhite = false;          // the transition goes to white instead of black (what a fade needs to know)
   bool toFrame = false;          // the transition goes to another frame (shown underneath)
   bool secondMoves = false;      // toFrame: the other frame comes in with the effect, in its pattern,
                                  // instead of staying in place underneath
   std::vector<uint8_t> shades;       // the original frame's shades, row-major (what Dust and Morph move)
   std::vector<uint8_t> shadesSecond; // toFrame: the shades of the other frame
   // A place (row * width + column) that shows the frame's background, UINT32_MAX for none; Morph
   // leaves the background where it is instead of making it black.
   uint32_t background = UINT32_MAX;
   uint32_t backgroundSecond = UINT32_MAX;
   // The colors of the places (one value each; what Morph pairs pixels by: white goes to white),
   // and which places are the same in both frames (1: same shade, same color), so that they stay.
   // Any of them may be empty (nothing is known: the shades are the colors, nothing is the same).
   std::vector<uint16_t> tones, tonesSecond;
   std::vector<uint8_t> same;
   uint32_t crossSpan = 0;        // toFrame: the largest color difference between the two frames, in
                                  // units of a 5-bit channel (what a cross-fade needs to know)
};

class Effect
{
public:
   virtual ~Effect() = default;

   // The most steps that still give frames that all differ (from each other and from the
   // starting frame). 0: the effect can do nothing with this frame.
   virtual uint32_t maxSteps() const = 0;
   // The fewest steps that make sense: 1, but the WPC pattern is always its 7 steps.
   virtual uint32_t minSteps() const { return 1; }
   // Why there is a most, in a few words, for the window that asks for the number of frames.
   virtual const char* limitReason() const = 0;

   // False for the effects that keep every pixel on the screen (the pixelate, the zoom in): they
   // only change what the pixels show, so nothing is taken away for a black or white fill or for
   // another frame to show through. In a transition to another frame they go through both pictures.
   virtual bool takesPixelsAway() const { return true; }

   // True for the effects that move single pixels around (Dust, Morph): frames then do not simply
   // lose pixels step by step, and their number of frames is found by trying.
   virtual bool movesPixels() const { return false; }

   // Where the pixel (x, y) of the new frame comes from (the identity by default).
   virtual Rule rule(uint32_t x, uint32_t y, uint32_t /*step*/, uint32_t /*steps*/) const
   {
      return {true, x, y};
   }

   // Effects that change colors (the fade) say so and give the new color of an RGB565 color
   // and of an original shade.
   virtual bool changesColors() const { return false; }
   virtual uint16_t color(uint16_t c, uint32_t /*step*/, uint32_t /*steps*/) const { return c; }
   virtual uint8_t shade(uint8_t s, uint32_t /*step*/, uint32_t /*steps*/) const { return s; }

   // Effects that go to another frame by blending (the fade) say so; the weight of the other frame
   // is then 0 .. 65536 (16.16), growing with the step, never 1.0 (the other frame is a frame of
   // its own). The others take pixels away, and the other frame shows where they were.
   virtual bool crossFades() const { return false; }
   virtual uint32_t weight(uint32_t /*step*/, uint32_t /*steps*/) const { return 0; }
};

// ---- Helpers --------------------------------------------------------------------------------

// A hash that spreads every input over 64 bits (splitmix64). The same on every compiler, so a
// random dissolve looks the same everywhere.
inline uint64_t mix64(uint64_t z)
{
   z += 0x9E3779B97F4A7C15ull;
   z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
   z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
   return z ^ (z >> 31);
}

// ---- Dissolve -------------------------------------------------------------------------------

// A 16x16 ordered-dither (Bayer) matrix, computed: every value 0 .. 255 appears exactly once,
// and the values are spread evenly, so thresholding it gives a regular, fine pattern.
inline uint32_t bayer16(uint32_t x, uint32_t y)
{
   x &= 15u;
   y &= 15u;
   const uint32_t diagonal = x ^ y;
   uint32_t value = 0;
   for (uint32_t bit = 0; bit < 4; ++bit)
   {
      const uint32_t shift = 2u * (3u - bit);
      value |= ((diagonal >> bit) & 1u) << (shift + 1u);
      value |= ((y >> bit) & 1u) << shift;
   }
   return value;
}

// Dissolve, Orderly: at step `step` of `steps` a share of about step / steps of the pixels is
// black, in the fixed pattern of the Bayer matrix. A pixel that is black at one step stays
// black at every later step. The last step keeps one pixel in 256.
class OrderlyDissolve final : public Effect
{
public:
   OrderlyDissolve(uint32_t width, uint32_t height) : m_pixels(static_cast<uint64_t>(width) * height) {}
   uint32_t maxSteps() const override { return static_cast<uint32_t>(std::min<uint64_t>(kMaxSteps, m_pixels)); }
   const char* limitReason() const override { return "the pattern has 255 steps"; }
   Rule rule(uint32_t x, uint32_t y, uint32_t step, uint32_t steps) const override
   {
      const uint32_t level = (255u * step + steps / 2u) / steps; // 0 .. 255, rounded
      return {bayer16(x, y) >= level, x, y};
   }

private:
   uint64_t m_pixels;
};

inline uint32_t ladderLevel(uint32_t levels, uint32_t step, uint32_t steps);

// The pattern many Bally/Williams (WPC) games dissolve with: the pixels are in eight groups, and every
// step takes one more group (an eighth of the picture) in a fixed dot pattern. The group of a pixel
// (0 goes first, 7 last) was read from a recording of the real transition: along a row it steps by 5
// (mod 8), the next block of eight pixels starts one higher, and the next row two lower.
inline uint32_t wpcGroup(uint32_t x, uint32_t y) { return (4u + 5u * (x & 7u) + (x >> 3) + 6u * y) & 7u; }

// WPC: at step `step` of `steps` the first groups are gone, so that the picture loses an eighth at a
// time; the last step keeps one group. With fewer steps, several groups go at once.
class WpcDissolve final : public Effect
{
public:
   uint32_t maxSteps() const override { return 7; }
   uint32_t minSteps() const override { return 7; }
   const char* limitReason() const override { return "the pattern has 8 groups of pixels"; }
   Rule rule(uint32_t x, uint32_t y, uint32_t step, uint32_t steps) const override
   {
      return {wpcGroup(x, y) >= ladderLevel(7, step, steps), x, y};
   }
};

// The order in which a dissolve takes the pixels.
enum class Order
{
   Random,    // a fixed shuffle
   CentreOut, // from the centre outward, on the frame's own proportions (an iris opening)
   EdgeIn,    // from the outside inward (an iris closing)
   Spiral,    // along a spiral from the top left corner, clockwise, to the centre
};

// A rank for every pixel (row-major): 0 goes first, width * height - 1 last. A permutation.
inline std::vector<uint32_t> makeRanks(Order order, uint32_t width, uint32_t height)
{
   const size_t count = static_cast<size_t>(width) * height;
   std::vector<uint32_t> ranks(count, 0);

   if (order == Order::Spiral)
   {
      // The usual spiral walk: the top row, down the right column, back along the bottom row,
      // up the left column, then the same one ring further in.
      int64_t top = 0, left = 0, bottom = static_cast<int64_t>(height) - 1,
              right = static_cast<int64_t>(width) - 1;
      uint32_t next = 0;
      auto at = [&](int64_t x, int64_t y) -> uint32_t& { return ranks[static_cast<size_t>(y) * width + static_cast<size_t>(x)]; };
      while (top <= bottom && left <= right)
      {
         for (int64_t x = left; x <= right; ++x)
            at(x, top) = next++;
         ++top;
         for (int64_t y = top; y <= bottom; ++y)
            at(right, y) = next++;
         --right;
         if (top <= bottom)
         {
            for (int64_t x = right; x >= left; --x)
               at(x, bottom) = next++;
            --bottom;
         }
         if (left <= right)
         {
            for (int64_t y = bottom; y >= top; --y)
               at(left, y) = next++;
            ++left;
         }
      }
      return ranks;
   }

   // The others sort the pixels by a key and break ties with a hash of the pixel's place, so
   // that pixels at the same distance do not go in a visible order.
   std::vector<uint64_t> key(count, 0);
   std::vector<uint64_t> tie(count, 0);
   uint64_t farthest = 0;
   for (uint32_t y = 0; y < height; ++y)
      for (uint32_t x = 0; x < width; ++x)
      {
         const size_t i = static_cast<size_t>(y) * width + x;
         tie[i] = mix64(i);
         if (order == Order::Random)
            continue;
         // The squared distance from the centre, with each axis in units of the frame's size,
         // times (width * height)^2, so that it is exact in integers: an ellipse that touches
         // all four edges, not a circle that hits the top and bottom first.
         const int64_t dx = (2 * static_cast<int64_t>(x) - (static_cast<int64_t>(width) - 1)) * height;
         const int64_t dy = (2 * static_cast<int64_t>(y) - (static_cast<int64_t>(height) - 1)) * width;
         key[i] = static_cast<uint64_t>(dx * dx + dy * dy);
         farthest = std::max(farthest, key[i]);
      }
   if (order == Order::EdgeIn)
      for (auto& k : key)
         k = farthest - k;

   std::vector<uint32_t> index(count);
   std::iota(index.begin(), index.end(), 0u);
   std::sort(index.begin(), index.end(), [&](uint32_t a, uint32_t b) {
      if (key[a] != key[b])
         return key[a] < key[b];
      if (tie[a] != tie[b])
         return tie[a] < tie[b];
      return a < b;
   });
   for (size_t rank = 0; rank < count; ++rank)
      ranks[index[rank]] = static_cast<uint32_t>(rank);
   return ranks;
}

// Dissolve with a given order. At step `step` of `steps` the first pixels of the order are
// black, so that the share grows evenly and the last step keeps one pixel in 256 (where the
// order ends: the corners for Centre-Out, the middle for Edge-In and Spiral).
class RankDissolve final : public Effect
{
public:
   RankDissolve(Order order, uint32_t width, uint32_t height)
      : m_width(width), m_ranks(makeRanks(order, width, height)),
        m_keep(std::max<uint64_t>(1, m_ranks.size() / 256))
   {
   }
   uint32_t maxSteps() const override
   {
      const uint64_t vanish = m_ranks.size() > m_keep ? m_ranks.size() - m_keep : 0;
      return static_cast<uint32_t>(std::min<uint64_t>(kMaxSteps, vanish));
   }
   const char* limitReason() const override { return "each step has to take at least one pixel"; }
   Rule rule(uint32_t x, uint32_t y, uint32_t step, uint32_t steps) const override
   {
      const uint64_t vanish = m_ranks.size() - m_keep;
      const uint64_t threshold = (vanish * step + steps / 2u) / steps;
      return {m_ranks[static_cast<size_t>(y) * m_width + x] >= threshold, x, y};
   }

private:
   uint32_t m_width;
   std::vector<uint32_t> m_ranks;
   uint64_t m_keep;
};

// ---- Steps through a ladder -----------------------------------------------------------------

// Some effects have a ladder of `levels` states, each one stronger than the one before (a bigger
// block, a stronger zoom, a wider wave). Frame `step` of `steps` takes the level `step * levels /
// steps`, rounded up: with at most `levels` steps every frame gets a level of its own, the last
// frame always gets the strongest level, and with more steps a level has to repeat.
inline uint32_t ladderLevel(uint32_t levels, uint32_t step, uint32_t steps)
{
   return static_cast<uint32_t>((static_cast<uint64_t>(step) * levels + steps - 1u) / steps);
}

// Where the other frame shows on a line of `extent` places when the first frame's picture is moved
// along the line by `shift` (positive: toward higher places) and the other frame follows right
// behind it, like the next picture of a film strip. `pos` is a place the first picture does not
// cover. A frame that has arrived stays at home, so a shift of more than a whole frame is the same
// as a shift of exactly one.
inline uint32_t followingPlace(uint32_t pos, uint32_t extent, int64_t shift)
{
   const int64_t e = static_cast<int64_t>(extent);
   const int64_t at = shift >= 0 ? static_cast<int64_t>(pos) + e - std::min(shift, e)
                                 : static_cast<int64_t>(pos) - e - std::max(shift, -e);
   return static_cast<uint32_t>(std::max<int64_t>(0, std::min(e - 1, at)));
}

// ---- Pixelate -------------------------------------------------------------------------------

// The picture turns into bigger and bigger blocks, each one showing the pixel at its middle. To
// another frame it goes through both: the first frame's blocks grow, then the other frame comes
// out of the largest blocks and shrinks back to pixels.
class Pixelate final : public Effect
{
public:
   Pixelate(uint32_t width, uint32_t height, bool toFrame)
      : m_width(width), m_height(height), m_toFrame(toFrame),
        m_blocks(std::max(2u, std::min(width, height)) - 1u) // block sizes 2 .. min(width, height)
   {
   }
   uint32_t maxSteps() const override { return std::min(kMaxSteps, levels()); }
   const char* limitReason() const override { return "the blocks can only grow so big"; }
   bool takesPixelsAway() const override { return false; }
   Rule rule(uint32_t x, uint32_t y, uint32_t step, uint32_t steps) const override
   {
      const uint32_t level = easedLevel(step, steps);
      const bool second = level > m_blocks;
      const uint32_t block = 1u + (second ? 2u * m_blocks + 1u - level : level); // 2 .. largest
      return {true, std::min(m_width - 1u, x / block * block + block / 2u),
              std::min(m_height - 1u, y / block * block + block / 2u), second};
   }

private:
   uint32_t levels() const { return m_toFrame ? 2u * m_blocks : m_blocks; }
   // The blocks start growing slowly and speed up (a square of the progress). Never slower than one
   // size per frame, so that every frame still differs from the one before.
   uint32_t easeIn(uint32_t n, uint32_t d, uint32_t span) const
   {
      return static_cast<uint32_t>((static_cast<uint64_t>(span) * n * n + static_cast<uint64_t>(d) * d - 1u) /
                                   (static_cast<uint64_t>(d) * d));
   }
   uint32_t easedLevel(uint32_t step, uint32_t steps) const
   {
      if (!m_toFrame)
         return std::min(m_blocks, std::max(step, easeIn(step, steps, m_blocks)));
      if (2u * step <= steps) // the first frame's blocks grow
         return std::min(m_blocks, std::max(step, easeIn(2u * step, steps, m_blocks)));
      const uint32_t back = steps - step; // the other frame's blocks shrink, slowly at the end
      const uint32_t size = std::max(back + 1u, easeIn(2u * back, steps, m_blocks));
      return 2u * m_blocks + 1u - std::min(size, m_blocks);
   }
   uint32_t m_width, m_height;
   bool m_toFrame;
   uint32_t m_blocks;
};

// ---- Zoom -----------------------------------------------------------------------------------

// Zoom Out: the picture shrinks toward the middle of the frame (nothing is shown around it, so a
// black or white fill, or the other frame, shows there). The most it shrinks is to 1/16 of the width.
class ZoomOut final : public Effect
{
public:
   ZoomOut(uint32_t width, uint32_t height)
      : m_width(width), m_height(height), m_smallest(std::max(2u, width / 16u))
   {
   }
   uint32_t maxSteps() const override { return m_width > m_smallest ? std::min(kMaxSteps, m_width - m_smallest) : 0; }
   const char* limitReason() const override { return "the picture shrinks one pixel of its width at a time"; }
   Rule rule(uint32_t x, uint32_t y, uint32_t step, uint32_t steps) const override
   {
      const uint32_t level = ladderLevel(m_width - m_smallest, step, steps);
      const uint32_t w = m_width - level; // the picture's size at this step
      const uint32_t h = std::max(1u, static_cast<uint32_t>((static_cast<uint64_t>(m_height) * w + m_width / 2u) / m_width));
      const uint32_t x0 = (m_width - w) / 2u;
      const uint32_t y0 = (m_height - h) / 2u;
      if (x < x0 || x >= x0 + w || y < y0 || y >= y0 + h)
         return {false, 0, 0};
      return {true, std::min(m_width - 1u, static_cast<uint32_t>(static_cast<uint64_t>(x - x0) * m_width / w)),
              std::min(m_height - 1u, static_cast<uint32_t>(static_cast<uint64_t>(y - y0) * m_height / h))};
   }

private:
   uint32_t m_width, m_height, m_smallest;
};

// Zoom In: the picture grows toward you, a smaller and smaller part of the middle fills the frame
// (down to 1/8 of the width). To another frame it zooms into the first frame, then the other
// frame zooms back out to full size.
class ZoomIn final : public Effect
{
public:
   ZoomIn(uint32_t width, uint32_t height, bool toFrame)
      : m_width(width), m_height(height), m_toFrame(toFrame), m_smallest(std::max(4u, width / 8u))
   {
   }
   uint32_t maxSteps() const override { return std::min(kMaxSteps, levels()); }
   const char* limitReason() const override { return "the picture grows one pixel of its width at a time"; }
   bool takesPixelsAway() const override { return false; }
   Rule rule(uint32_t x, uint32_t y, uint32_t step, uint32_t steps) const override
   {
      const uint32_t depth = m_width > m_smallest ? m_width - m_smallest : 0;
      const uint32_t level = ladderLevel(levels(), step, steps);
      const bool second = level > depth;
      const uint32_t zoom = second ? 2u * depth + 1u - level : level; // 1 .. depth
      const uint32_t w = m_width - zoom; // the part of the picture that fills the frame
      const uint32_t h = std::max(2u, static_cast<uint32_t>((static_cast<uint64_t>(m_height) * w + m_width / 2u) / m_width));
      const uint32_t x0 = (m_width - w) / 2u;
      const uint32_t y0 = m_height > h ? (m_height - h) / 2u : 0u;
      return {true, std::min(m_width - 1u, x0 + static_cast<uint32_t>(static_cast<uint64_t>(x) * w / m_width)),
              std::min(m_height - 1u, y0 + static_cast<uint32_t>(static_cast<uint64_t>(y) * h / m_height)), second};
   }

private:
   uint32_t levels() const
   {
      const uint32_t depth = m_width > m_smallest ? m_width - m_smallest : 0;
      return m_toFrame ? 2u * depth : depth;
   }
   uint32_t m_width, m_height;
   bool m_toFrame;
   uint32_t m_smallest;
};

// ---- Wave -----------------------------------------------------------------------------------

// A wave of whole numbers: `amplitude` times a smooth wave of `period` (a multiple of 4) places,
// rounded to whole pixels. Two parabolas, one up and one down, so that it is exact in integers
// and the same on every compiler.
inline int32_t waveShift(uint32_t place, uint32_t period, uint32_t amplitude)
{
   const uint32_t half = period / 2u;
   const uint32_t at = place % period;
   const bool down = at >= half;
   const uint64_t q = down ? at - half : at;
   const uint64_t hump = 4ull * q * (half - q); // 0 .. half * half (reached at the quarter)
   const int64_t magnitude = static_cast<int64_t>((hump * amplitude + static_cast<uint64_t>(half) * half / 2u) /
                                                  (static_cast<uint64_t>(half) * half));
   return static_cast<int32_t>(down ? -magnitude : magnitude);
}

// How fast the columns of a melt drip: a number 0 .. 255 for every place along the line, that
// looks random but is the same on every compiler. Neighbours go in groups of four with a little
// jitter, like paint that drips in streaks, and the table is stretched so that the slowest place
// is exactly 0 and the fastest exactly 255: one place is always the last to leave.
inline std::vector<uint8_t> dripSpeeds(uint32_t extent)
{
   std::vector<uint32_t> raw(extent);
   for (uint32_t i = 0; i < extent; ++i)
      raw[i] = (3u * static_cast<uint32_t>(mix64(i / 4u * 2u + 1u) & 255u) + static_cast<uint32_t>(mix64(i * 2u) & 255u)) / 4u;
   const uint32_t lo = extent ? *std::min_element(raw.begin(), raw.end()) : 0u;
   const uint32_t hi = extent ? *std::max_element(raw.begin(), raw.end()) : 0u;
   std::vector<uint8_t> out(extent, 0);
   for (uint32_t i = 0; i < extent; ++i)
      out[i] = hi > lo ? static_cast<uint8_t>((raw[i] - lo) * 255u / (hi - lo)) : 0u;
   return out;
}

// Wave and Melt: every row (Horizontal: slides to the right) or column (Vertical: slides down) is
// pulled out of the frame, each by a different amount, so the picture turns into a curtain that
// is pulled out. The slowest rows are pulled one pixel per step, the others up to twice as far, and
// a thin sliver of the slowest is left at the end. What it uncovers is black (or white, or the
// other frame, which can follow behind in the same pattern).
//   Wave: the speeds follow a wave: one over the height for the rows, four over the width for the columns.
//   Drips: the speeds look random, like paint dripping (Melt).
class Curtain final : public Effect
{
public:
   enum class Speeds { Wave, Drips };
   Curtain(bool horizontal, Speeds speeds, uint32_t width, uint32_t height, bool follow = false)
      : m_horizontal(horizontal), m_speeds(speeds), m_width(width), m_height(height), m_follow(follow)
   {
      const uint32_t across = horizontal ? height : width; // the pattern runs along this axis
      m_period = std::max(8u, (horizontal ? across : across / 4u) / 4u * 4u);
      if (speeds == Speeds::Drips)
         m_drips = dripSpeeds(across);
   }
   uint32_t maxSteps() const override { return std::min(kMaxSteps, levels()); }
   const char* limitReason() const override { return "the picture moves one pixel at a time"; }
   Rule rule(uint32_t x, uint32_t y, uint32_t step, uint32_t steps) const override
   {
      // Every row is pulled out by `pull` at least; the pattern adds up to the same again.
      const uint32_t pull = ladderLevel(levels(), step, steps);
      const uint32_t along = m_horizontal ? y : x; // the place in the pattern
      const uint32_t pos = m_horizontal ? x : y;   // the place on the line that is pulled
      const uint32_t extent = m_horizontal ? m_width : m_height;
      int64_t shift = pull;
      if (m_speeds == Speeds::Wave)
      {
         const uint32_t amplitude = pull / 2u;
         shift += amplitude + waveShift(along, m_period, amplitude);
      }
      else
         shift += static_cast<int64_t>(pull) * m_drips[along] / 256;
      const int64_t from = static_cast<int64_t>(pos) - shift;
      if (from >= 0 && from < extent)
         return m_horizontal ? Rule{true, static_cast<uint32_t>(from), y} : Rule{true, x, static_cast<uint32_t>(from)};
      if (!m_follow)
         return {false, 0, 0};
      const uint32_t other = followingPlace(pos, extent, shift);
      return m_horizontal ? Rule{true, other, y, true} : Rule{true, x, other, true};
   }

private:
   // The slowest rows are pulled out one pixel per step, until one column (or row) is left.
   uint32_t levels() const
   {
      const uint32_t extent = m_horizontal ? m_width : m_height;
      return extent > 1u ? extent - 1u : 0u;
   }
   bool m_horizontal;
   Speeds m_speeds;
   uint32_t m_width, m_height;
   bool m_follow;
   uint32_t m_period = 8;
   std::vector<uint8_t> m_drips;
};

// ---- Checkerboard ---------------------------------------------------------------------------

// Checkerboard: the frame is cut into squares, and they disappear one by one: first the squares of
// one color of a checkerboard, in a sweep from the top left corner, then the others. One square is
// left at the end.
class Checkerboard final : public Effect
{
public:
   Checkerboard(uint32_t size, uint32_t width, uint32_t height)
      : m_size(std::max(1u, size)), m_across((width + m_size - 1u) / m_size)
   {
      const uint32_t down = (height + m_size - 1u) / m_size;
      const size_t count = static_cast<size_t>(m_across) * down;
      std::vector<uint32_t> index(count);
      std::iota(index.begin(), index.end(), 0u);
      auto key = [&](uint32_t i) {
         const uint32_t tx = i % m_across, ty = i / m_across;
         return std::make_tuple((tx + ty) & 1u, tx + ty, ty);
      };
      std::sort(index.begin(), index.end(), [&](uint32_t a, uint32_t b) { return key(a) < key(b); });
      m_ranks.assign(count, 0);
      for (size_t rank = 0; rank < count; ++rank)
         m_ranks[index[rank]] = static_cast<uint32_t>(rank);
   }
   uint32_t maxSteps() const override
   {
      return m_ranks.size() > 1 ? static_cast<uint32_t>(std::min<uint64_t>(kMaxSteps, m_ranks.size() - 1u)) : 0;
   }
   const char* limitReason() const override { return "each step has to take at least one square"; }
   Rule rule(uint32_t x, uint32_t y, uint32_t step, uint32_t steps) const override
   {
      const uint64_t vanish = m_ranks.size() - 1u;
      const uint64_t threshold = (vanish * step + steps / 2u) / steps;
      const size_t tile = static_cast<size_t>(y / m_size) * m_across + x / m_size;
      return {m_ranks[tile] >= threshold, x, y};
   }

private:
   uint32_t m_size, m_across;
   std::vector<uint32_t> m_ranks;
};

// ---- Moving along an axis -------------------------------------------------------------------

enum class Direction
{
   Up,
   Down,
   Left,
   Right,
};

// Everything that moves leaves a thin strip of the picture behind at the end: 1/32 of the axis,
// at least one pixel. The pieces move in whole pixels, so the travel is also the most steps.
inline uint32_t tailOf(uint32_t extent) { return std::max(1u, extent / 32u); }
inline uint32_t travelOf(uint32_t extent)
{
   const uint32_t tail = tailOf(extent);
   return extent > tail ? extent - tail : 0u;
}
inline uint32_t offsetAt(uint32_t extent, uint32_t step, uint32_t steps)
{
   return static_cast<uint32_t>((static_cast<uint64_t>(travelOf(extent)) * step + steps / 2u) / steps);
}

inline bool isHorizontalMove(Direction d) { return d == Direction::Left || d == Direction::Right; }

// One pixel of a picture moved by `offset` in a direction, on a width x height frame. What the
// move uncovers is nothing, or the other frame following behind (`follow`).
inline Rule movedPixel(Direction d, uint32_t x, uint32_t y, uint32_t offset, uint32_t width, uint32_t height,
                       bool follow = false)
{
   const int64_t o = offset;
   switch (d)
   {
   case Direction::Left:
      if (x + offset < width)
         return {true, x + offset, y};
      return follow ? Rule{true, followingPlace(x, width, -o), y, true} : Rule{false, 0, 0};
   case Direction::Right:
      if (x >= offset)
         return {true, x - offset, y};
      return follow ? Rule{true, followingPlace(x, width, o), y, true} : Rule{false, 0, 0};
   case Direction::Up:
      if (y + offset < height)
         return {true, x, y + offset};
      return follow ? Rule{true, x, followingPlace(y, height, -o), true} : Rule{false, 0, 0};
   case Direction::Down:
      if (y >= offset)
         return {true, x, y - offset};
      return follow ? Rule{true, x, followingPlace(y, height, o), true} : Rule{false, 0, 0};
   }
   return {false, 0, 0};
}

// Slide: the whole picture slides off the screen in the direction.
class Slide final : public Effect
{
public:
   Slide(Direction direction, uint32_t width, uint32_t height, bool follow = false)
      : m_direction(direction), m_width(width), m_height(height), m_follow(follow)
   {
   }
   uint32_t maxSteps() const override { return std::min(kMaxSteps, travelOf(extent())); }
   const char* limitReason() const override { return "the picture moves in whole pixels"; }
   Rule rule(uint32_t x, uint32_t y, uint32_t step, uint32_t steps) const override
   {
      return movedPixel(m_direction, x, y, offsetAt(extent(), step, steps), m_width, m_height, m_follow);
   }

private:
   uint32_t extent() const { return isHorizontalMove(m_direction) ? m_width : m_height; }
   Direction m_direction;
   uint32_t m_width, m_height;
   bool m_follow;
};

// Wipe: a black bar moves over the picture in the direction (Left: it comes in from the right
// edge and moves left). The picture itself stays where it is.
class Wipe final : public Effect
{
public:
   Wipe(Direction direction, uint32_t width, uint32_t height)
      : m_direction(direction), m_width(width), m_height(height)
   {
   }
   uint32_t maxSteps() const override { return std::min(kMaxSteps, travelOf(extent())); }
   const char* limitReason() const override { return "the bar moves in whole pixels"; }
   Rule rule(uint32_t x, uint32_t y, uint32_t step, uint32_t steps) const override
   {
      const uint32_t covered = offsetAt(extent(), step, steps);
      bool visible = true;
      switch (m_direction)
      {
      case Direction::Left: visible = x < m_width - covered; break;
      case Direction::Right: visible = x >= covered; break;
      case Direction::Up: visible = y < m_height - covered; break;
      case Direction::Down: visible = y >= covered; break;
      }
      return {visible, x, y};
   }

private:
   uint32_t extent() const { return isHorizontalMove(m_direction) ? m_width : m_height; }
   Direction m_direction;
   uint32_t m_width, m_height;
};

// Alternating Slide: every other line slides one way, the lines between slide the other way.
// Horizontal: the 1st, 3rd, 5th ... row slide left and the 2nd, 4th ... row slide right.
// Vertical: the 1st, 3rd ... column go up and the 2nd, 4th ... column go down.
class AlternatingSlide final : public Effect
{
public:
   AlternatingSlide(bool rows, uint32_t width, uint32_t height, bool follow = false)
      : m_rows(rows), m_width(width), m_height(height), m_follow(follow)
   {
   }
   uint32_t maxSteps() const override { return std::min(kMaxSteps, travelOf(extent())); }
   const char* limitReason() const override { return "the lines move in whole pixels"; }
   Rule rule(uint32_t x, uint32_t y, uint32_t step, uint32_t steps) const override
   {
      const uint32_t offset = offsetAt(extent(), step, steps);
      // Counted as people count: the first row (y = 0) is an odd one.
      const bool odd = ((m_rows ? y : x) % 2u) == 0u;
      const Direction d = m_rows ? (odd ? Direction::Left : Direction::Right)
                                 : (odd ? Direction::Up : Direction::Down);
      return movedPixel(d, x, y, offset, m_width, m_height, m_follow);
   }

private:
   uint32_t extent() const { return m_rows ? m_width : m_height; }
   bool m_rows;
   uint32_t m_width, m_height;
   bool m_follow;
};

// Split: the picture is cut in the middle; the two halves move apart (vertical: a vertical cut,
// the left half to the left and the right half to the right; horizontal: the top half up and
// the bottom half down) until a thin strip of each is left.
inline uint32_t splitTravel(uint32_t extent)
{
   const uint32_t half = extent / 2u;
   const uint32_t tail = std::max(1u, half / 32u);
   return half > tail ? half - tail : 0u;
}

inline uint32_t splitShift(uint32_t extent, uint32_t step, uint32_t steps)
{
   return static_cast<uint32_t>((static_cast<uint64_t>(splitTravel(extent)) * step + steps / 2u) / steps);
}

// One axis of a split: the first half moves toward 0 and the second half toward the end by
// `shift`; between them is black. Returns the position the pixel shows, or false.
inline bool splitAxis(uint32_t pos, uint32_t extent, uint32_t shift, uint32_t& source)
{
   const uint32_t half = extent / 2u;
   if (pos + shift < half)
   {
      source = pos + shift;
      return true;
   }
   if (pos >= half + shift)
   {
      source = pos - shift;
      return true;
   }
   return false;
}

class Split final : public Effect
{
public:
   Split(bool vertical, uint32_t width, uint32_t height) : m_vertical(vertical), m_width(width), m_height(height) {}
   uint32_t maxSteps() const override { return std::min(kMaxSteps, splitTravel(extent())); }
   const char* limitReason() const override { return "the halves move in whole pixels"; }
   Rule rule(uint32_t x, uint32_t y, uint32_t step, uint32_t steps) const override
   {
      uint32_t source = 0;
      if (!splitAxis(m_vertical ? x : y, extent(), splitShift(extent(), step, steps), source))
         return {false, 0, 0};
      return m_vertical ? Rule{true, source, y} : Rule{true, x, source};
   }

private:
   uint32_t extent() const { return m_vertical ? m_width : m_height; }
   bool m_vertical;
   uint32_t m_width, m_height;
};

// ---- Blinds ---------------------------------------------------------------------------------

// Black bars at regular intervals that grow wider until only a thin slit of the picture is left
// in each interval. Horizontal: horizontal bars (stacked, as in window blinds). Vertical:
// vertical bars side by side. The bars grow in whole pixels, so the most steps is the interval
// less one.
class Blinds final : public Effect
{
public:
   Blinds(bool horizontal, uint32_t width, uint32_t height)
      : m_horizontal(horizontal), m_pitch(std::max(3u, horizontal ? height / 4u : width / 8u))
   {
   }
   uint32_t maxSteps() const override { return std::min(kMaxSteps, m_pitch - 1u); }
   const char* limitReason() const override { return "each bar grows in whole pixels"; }
   Rule rule(uint32_t x, uint32_t y, uint32_t step, uint32_t steps) const override
   {
      const uint32_t within = (m_horizontal ? y : x) % m_pitch;
      const uint32_t bar = ((m_pitch - 1u) * step + steps / 2u) / steps; // the bar's width
      const uint32_t start = (m_pitch - bar) / 2u;                      // centred in the interval
      return {!(within >= start && within < start + bar), x, y};
   }

private:
   bool m_horizontal;
   uint32_t m_pitch;
};

// ---- Fade -----------------------------------------------------------------------------------

// Fade: the picture gets darker (To Black) or brighter (To White) a little more at each step.
// The brightest color gets one unit away from black (or the darkest one unit from white) at the
// last step, so the last frame is almost black (almost white), never exactly. The geometry does
// not change.
class Fade final : public Effect
{
public:
   Fade(bool toWhite, const SourceInfo& source) : m_toWhite(toWhite)
   {
      if (source.toFrame)
      {
         // A cross-fade to another frame: each step has to move every pixel at least one unit (of
         // a 5-bit channel) from the one before, and the last has to differ from the other frame.
         m_cross = true;
         m_maxSteps = source.crossSpan >= 2 ? std::min(kMaxSteps, source.crossSpan - 1u) : 0;
         return;
      }
      // The room a color has to fade in, in units of a 5-bit channel (green has 6 bits, so it
      // counts half): the brightest color's brightest channel for a fade to black, the darkest
      // color's darkest channel for a fade to white.
      uint32_t span = 0;
      for (uint16_t c : source.colors)
         span = std::max(span, spanOf(c));
      m_colorSpan = span;

      // The colors that change first, for the quick check below.
      m_colors = source.colors;
      std::sort(m_colors.begin(), m_colors.end(), [&](uint16_t a, uint16_t b) { return spanOf(a) > spanOf(b); });
      m_maxSteps = computeMaxSteps();
   }

   uint32_t maxSteps() const override { return m_maxSteps; }
   const char* limitReason() const override
   {
      return m_cross ? "the two frames only differ by so many brightness levels"
                     : "the picture only has so many brightness levels to fade through";
   }
   bool changesColors() const override { return !m_cross; }
   bool crossFades() const override { return m_cross; }
   uint32_t weight(uint32_t step, uint32_t steps) const override
   {
      return static_cast<uint32_t>(static_cast<uint64_t>(step) * 65536u / (steps + 1u));
   }

   uint16_t color(uint16_t c, uint32_t step, uint32_t steps) const override
   {
      const uint32_t f = factor(m_colorSpan, step, steps);
      const uint32_t r = (c >> 11) & 31u, g = (c >> 5) & 63u, b = c & 31u;
      return static_cast<uint16_t>((channel(r, 31, f) << 11) | (channel(g, 63, f) << 5) | channel(b, 31, f));
   }

private:
   // How far a color is from the end of its fade, in 5-bit units: its brightest channel (to
   // black) or the darkest channel's distance from the top (to white).
   uint32_t spanOf(uint16_t c) const
   {
      const uint32_t r = (c >> 11) & 31u, g = (c >> 5) & 63u, b = c & 31u;
      if (m_toWhite)
         return std::max({31u - r, (63u - g + 1u) / 2u, 31u - b});
      return std::max({r, (g + 1u) / 2u, b});
   }

   // 16.16 fixed point: 1.0 at step 0 down to 1 / span at the last step.
   static uint32_t factor(uint32_t span, uint32_t step, uint32_t steps)
   {
      if (span == 0)
         return 65536u;
      const uint64_t last = 65536u / span;
      const uint64_t drop = ((65536u - last) * step + steps / 2u) / steps;
      return static_cast<uint32_t>(65536u - drop);
   }

   // A value of a channel that goes from 0 to `top`, faded by the factor: toward 0 for a fade
   // to black, toward `top` for a fade to white.
   uint32_t channel(uint32_t value, uint32_t top, uint32_t f) const
   {
      if (m_toWhite)
         return top - (((top - value) * f + 32768u) >> 16);
      return (value * f + 32768u) >> 16;
   }

   uint32_t computeMaxSteps() const
   {
      if (m_colorSpan == 0 || m_colors.empty())
         return 0; // a picture that is already all black (white): nothing to fade
      for (uint32_t n = kMaxSteps; n >= 1; --n)
      {
         // The fade only goes one way, so frames differ from each other when each differs
         // from the one before it, and the first from the starting frame.
         bool distinct = true;
         for (uint32_t s = 0; s < n && distinct; ++s)
         {
            bool differs = false;
            for (uint16_t c : m_colors)
               if (color(c, s, n) != color(c, s + 1, n))
               {
                  differs = true;
                  break;
               }
            distinct = differs;
         }
         if (distinct)
            return n;
      }
      return 0;
   }

   bool m_toWhite;
   bool m_cross = false;
   uint32_t m_colorSpan = 0;
   std::vector<uint16_t> m_colors;
   uint32_t m_maxSteps = 0;
};

// ---- Pixel particles (Dust, Morph) ----------------------------------------------------------

// A random number 0 .. 1 (exclusive) from a hash and a salt.
inline double unitOf(uint64_t hash, uint32_t salt)
{
   return static_cast<double>((hash >> (16u * salt)) & 0xFFFFu) / 65536.0;
}

// A triangle wave of a phase (any number): 0 at 0, 1 at 1/4, 0 at 1/2, -1 at 3/4. Only additions and
// multiplications, so it is the same on every compiler.
inline double triangle(double phase)
{
   const double f = phase - std::floor(phase);
   return f < 0.25 ? 4.0 * f : f < 0.75 ? 2.0 - 4.0 * f : 4.0 * f - 4.0;
}

inline int32_t rounded(double v) { return static_cast<int32_t>(std::floor(v + 0.5)); }

// How far along a direction a place is: 0 at the side the movement starts, 1 at the other.
inline double sweepPosition(Direction d, uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
   const double ux = width > 1 ? static_cast<double>(x) / (width - 1) : 0.0;
   const double uy = height > 1 ? static_cast<double>(y) / (height - 1) : 0.0;
   switch (d)
   {
   case Direction::Right: return ux;
   case Direction::Left: return 1.0 - ux;
   case Direction::Down: return uy;
   default: return 1.0 - uy;
   }
}

// The frame as the pixel effects see it: which places are lit (brighter than the darkest shade
// of the picture), and how bright.
inline std::vector<uint32_t> litPlaces(const std::vector<uint8_t>& shades, uint32_t width, uint32_t height)
{
   std::vector<uint32_t> out;
   if (shades.size() != static_cast<size_t>(width) * height || shades.empty())
      return out;
   const uint8_t darkest = *std::min_element(shades.begin(), shades.end());
   for (size_t i = 0; i < shades.size(); ++i)
      if (shades[i] > darkest)
         out.push_back(static_cast<uint32_t>(i));
   return out;
}

// What the effects that move single pixels share: a map of what every place shows at one step,
// built when the step changes, and the number of frames, found by trying.
class PixelEffect : public Effect
{
public:
   bool movesPixels() const final { return true; }

   uint32_t maxSteps() const final
   {
      if (!m_max)
         m_max = findMax();
      return *m_max;
   }
   Rule rule(uint32_t x, uint32_t y, uint32_t step, uint32_t steps) const final
   {
      if (x >= m_width || y >= m_height)
         return {false, 0, 0};
      return mapOf(step, steps)[static_cast<size_t>(y) * m_width + x];
   }

protected:
   PixelEffect(uint32_t width, uint32_t height) : m_width(width), m_height(height) {}
   // Whether there is anything to move.
   virtual bool ready() const = 0;
   // Fills the map: what each place shows at `step` of `steps`.
   virtual void build(std::vector<Rule>& cells, uint32_t step, uint32_t steps) const = 0;

   uint32_t m_width, m_height;

private:
   const std::vector<Rule>& mapOf(uint32_t step, uint32_t steps) const
   {
      if (!m_valid || m_step != step || m_steps != steps)
      {
         m_cells.assign(static_cast<size_t>(m_width) * m_height, Rule{false, 0, 0});
         build(m_cells, step, steps);
         m_step = step;
         m_steps = steps;
         m_valid = true;
      }
      return m_cells;
   }
   // The most frames whose pictures all differ from each other and from the starting frame, among
   // a few round numbers (the frames get closer together as there are more of them).
   uint32_t findMax() const
   {
      if (!ready())
         return 0;
      auto hashOf = [&](const std::vector<Rule>& cells) {
         uint64_t h = 0x1234567ull;
         for (const Rule& r : cells)
            h = mix64(h ^ (r.visible ? (1ull << 40) | (r.second ? 1ull << 41 : 0ull) | (static_cast<uint64_t>(r.x) << 20) | r.y
                                     : 0ull));
         return h;
      };
      std::vector<Rule> plain(static_cast<size_t>(m_width) * m_height);
      for (uint32_t y = 0; y < m_height; ++y)
         for (uint32_t x = 0; x < m_width; ++x)
            plain[static_cast<size_t>(y) * m_width + x] = Rule{true, x, y};
      const uint64_t start = hashOf(plain);
      for (uint32_t n : {64u, 48u, 36u, 30u, 24u, 18u, 12u, 9u, 6u, 4u, 3u, 2u, 1u})
      {
         std::vector<uint64_t> seen{start};
         bool good = true;
         for (uint32_t step = 1; step <= n && good; ++step)
         {
            std::vector<Rule> cells(plain.size(), Rule{false, 0, 0});
            build(cells, step, n);
            const uint64_t h = hashOf(cells);
            good = std::find(seen.begin(), seen.end(), h) == seen.end();
            seen.push_back(h);
         }
         if (good)
            return n;
      }
      return 0;
   }

   mutable std::vector<Rule> m_cells;
   mutable uint32_t m_step = 0, m_steps = 0;
   mutable bool m_valid = false;
   mutable std::optional<uint32_t> m_max;
};

// ---- Dust -----------------------------------------------------------------------------------

// Dust: the picture crumbles into single pixels and the wind blows them away. A wave of wind
// sweeps over the picture from the side it blows from; every lit pixel waits for the wind, then
// flies off, faster and faster, drifting up and wobbling, and some of them are gone on the way.
// The pixels of the darkest shade just vanish as the wind passes. What is left is black (or the
// other frame, which shows underneath).
class Dust final : public PixelEffect
{
public:
   Dust(Direction wind, const std::vector<uint8_t>& shades, uint32_t width, uint32_t height)
      : PixelEffect(width, height), m_wind(wind), m_shades(shades)
   {
      m_lit = litPlaces(shades, width, height);
      m_darkest = shades.empty() ? 0 : *std::min_element(shades.begin(), shades.end());
      if (shades.size() != static_cast<size_t>(width) * height)
      {
         m_shades.assign(static_cast<size_t>(width) * height, 0);
         m_lit.clear();
      }
      m_parts.resize(shades.size());
      const bool horizontal = isHorizontalMove(wind);
      const double along = horizontal ? width : height;  // the length the wind blows over
      const double across = horizontal ? height : width; // the other way
      for (size_t i = 0; i < m_parts.size(); ++i)
      {
         const uint32_t x = static_cast<uint32_t>(i % width), y = static_cast<uint32_t>(i / width);
         const uint64_t h = mix64(0xD057ull * 0x10001ull + i);
         Part& p = m_parts[i];
         p.start = 0.5 * sweepPosition(wind, x, y, width, height) + 0.12 * unitOf(h, 0);
         p.flight = 0.30 + 0.12 * unitOf(h, 1);
         p.speed = along * (0.31 + 0.69 * unitOf(h, 2));
         p.life = 0.55 + 0.45 * unitOf(h, 3);
         p.rise = across * (0.12 + 0.40 * unitOf(h, 3));
         p.wobble = (1.5 + 3.5 * unitOf(h, 2)) * across / 32.0;
         p.phase = unitOf(h, 3);
      }
      // The first lit pixel starts at once, and the picture is gone when the last one has flown its flight.
      double first = 1e9;
      for (size_t i = 0; i < m_parts.size(); ++i)
         if (isLit(i))
            first = std::min(first, m_parts[i].start);
      for (Part& p : m_parts)
         p.start -= first < 1e9 ? first : 0.0;
      // The one that flies last is a speck that stays on the screen, drifting slowly, until the end:
      // the last frame is never empty.
      // It is one of the lit pixels with room to fly, so that it keeps moving until the end.
      auto room = [&](size_t i) {
         const double x = static_cast<double>(i % width), y = static_cast<double>(i / width);
         switch (wind)
         {
         case Direction::Right: return width - 1 - x;
         case Direction::Left: return x;
         case Direction::Down: return height - 1 - y;
         default: return y;
         }
      };
      size_t last = 0;
      bool found = false;
      for (size_t i = 0; i < m_parts.size(); ++i)
         if (isLit(i) && room(i) >= 0.4 * along &&
             (!found || m_parts[i].start + m_parts[i].flight > m_parts[last].start + m_parts[last].flight))
         {
            last = i;
            found = true;
         }
      for (size_t i = 0; i < m_parts.size() && !found; ++i) // no such pixel: the one with the most room
         if (isLit(i) && (!isLit(last) || room(i) > room(last)))
            last = i;
      m_parts[last].life = 2.0;
      m_parts[last].speed = room(last) * 0.9;
      m_parts[last].rise = 0;
      m_parts[last].wobble = 0;
      m_parts[last].lingers = true;
      m_end = 0.0;
      for (size_t i = 0; i < m_parts.size(); ++i)
         if (isLit(i) && !m_parts[i].lingers)
            m_end = std::max(m_end, m_parts[i].start + m_parts[i].flight * std::min(1.0, m_parts[i].life));
      // It flies until the others are gone, so that it is still moving in the last frame.
      m_parts[last].flight = std::max(m_parts[last].flight, m_end - m_parts[last].start);
      m_end = m_parts[last].start + m_parts[last].flight;
   }
   const char* limitReason() const override
   {
      return ready() ? "the pixels' flights end at different moments" : "the frame has no lit pixels";
   }

protected:
   bool ready() const override { return !m_lit.empty(); }
   void build(std::vector<Rule>& cells, uint32_t step, uint32_t steps) const override
   {
      const double t = m_end * static_cast<double>(step) / steps;
      // Which direction the wind blows, and which way the pixels drift up from the ground.
      double dx = 0, dy = 0, ux = 0, uy = -1;
      switch (m_wind)
      {
      case Direction::Right: dx = 1; break;
      case Direction::Left: dx = -1; break;
      case Direction::Down: dy = 1; ux = -1; uy = 0; break;
      default: dy = -1; ux = -1; uy = 0; break;
      }
      std::vector<int> rank(cells.size(), -2);
      for (size_t i = 0; i < m_parts.size(); ++i)
      {
         const Part& p = m_parts[i];
         const uint32_t x = static_cast<uint32_t>(i % m_width), y = static_cast<uint32_t>(i / m_width);
         int32_t px = static_cast<int32_t>(x), py = static_cast<int32_t>(y);
         int level;
         const bool lit = isLit(i);
         if (lit)
         {
            level = m_shades[i];
            double l = (t - p.start) / p.flight;
            if (p.lingers)
               l = std::min(l, 0.999);
            else if (l >= p.life || l >= 1.0)
               continue; // gone
            if (l > 0)
            {
               const double run = p.speed * (0.35 * l + 0.65 * l * l);
               const double lift = p.rise * l;
               const double sway = p.wobble * triangle(p.phase + 1.4 * l) * l;
               px = rounded(x + run * dx + (lift + sway) * ux);
               py = rounded(y + run * dy + (lift + sway) * uy);
            }
         }
         else
         {
            level = -1;
            if (t >= p.start)
               continue; // the wind has passed
         }
         if (lit && p.lingers) // the last speck stays on the screen, at the edge if it has to
         {
            px = std::clamp(px, 0, static_cast<int32_t>(m_width) - 1);
            py = std::clamp(py, 0, static_cast<int32_t>(m_height) - 1);
         }
         if (px < 0 || py < 0 || px >= static_cast<int32_t>(m_width) || py >= static_cast<int32_t>(m_height))
            continue; // blown out of the frame
         const size_t at = static_cast<size_t>(py) * m_width + px;
         if (level > rank[at])
         {
            rank[at] = level;
            cells[at] = Rule{true, x, y};
         }
      }
   }

private:
   bool isLit(size_t i) const { return m_shades[i] > m_darkest; }
   struct Part
   {
      double start = 0, flight = 0.3, speed = 0, life = 1, rise = 0, wobble = 0, phase = 0;
      bool lingers = false; // stays until the last frame
   };
   Direction m_wind;
   std::vector<uint8_t> m_shades;
   std::vector<uint32_t> m_lit;
   uint8_t m_darkest = 0;
   double m_end = 0.01;
   std::vector<Part> m_parts;
};

// ---- Morph ----------------------------------------------------------------------------------

// Morph: the lit pixels of the first frame travel to the lit pixels of the other frame, each one
// to a partner chosen so that the pixels hardly cross (both pictures are cut in halves again and
// again, along their longer side, and the halves are paired). On the way a pixel changes from its
// old color to its new one. Everything else is black. Random: the pixels leave at random moments.
// Sweep: they leave in a wave from the side the sweep starts at.
class Morph final : public PixelEffect
{
public:
   // The nearest place in the row (then column) that has only background in the frame, UINT32_MAX if
   // there is none.
   static uint32_t nearestBackground(const std::vector<uint8_t>& shades, uint8_t darkest, uint32_t width,
                                     uint32_t height, int64_t x, int64_t y)
   {
      auto unlit = [&](int64_t px, int64_t py) { return shades[static_cast<size_t>(py) * width + px] <= darkest; };
      for (int64_t d = 0; d < std::max<int64_t>(width, height); ++d)
      {
         if (x - d >= 0 && unlit(x - d, y))
            return static_cast<uint32_t>(y * width + x - d);
         if (x + d < width && unlit(x + d, y))
            return static_cast<uint32_t>(y * width + x + d);
         if (y - d >= 0 && unlit(x, y - d))
            return static_cast<uint32_t>((y - d) * width + x);
         if (y + d < height && unlit(x, y + d))
            return static_cast<uint32_t>((y + d) * width + x);
      }
      return UINT32_MAX;
   }

   // What every place shows when no pixel is on it, early (before the halfway point) and late. Where
   // a frame has only background, that is the place's own pixel in that frame, the first frame's
   // early and the other frame's late; where it has none, the other frame's own pixel if that has
   // background there (the sky behind the cow is still there, though the cow's pixels are not yet);
   // where both frames have a pixel, the nearest background pixel of the row.
   void makeEmpties()
   {
      const size_t cells = static_cast<size_t>(m_width) * m_height;
      m_emptyEarly.assign(cells, Rule{false, 0, 0, false, true}); // black, whatever the other frame shows
      m_emptyLate = m_emptyEarly;
      if (m_from.size() != cells || m_to.size() != cells || cells == 0)
         return;
      const uint8_t darkFrom = *std::min_element(m_from.begin(), m_from.end());
      const uint8_t darkTo = *std::min_element(m_to.begin(), m_to.end());
      for (uint32_t y = 0; y < m_height; ++y)
         for (uint32_t x = 0; x < m_width; ++x)
         {
            const size_t i = static_cast<size_t>(y) * m_width + x;
            const bool unlitFrom = m_from[i] <= darkFrom, unlitTo = m_to[i] <= darkTo;
            const Rule own{true, x, y, false}, ownTo{true, x, y, true};
            auto near = [&](const std::vector<uint8_t>& shades, uint8_t dark, uint32_t place, bool second) {
               const uint32_t f = nearestBackground(shades, dark, m_width, m_height, x, y);
               const uint32_t g = f != UINT32_MAX ? f : place;
               return g == UINT32_MAX || g >= cells ? Rule{false, 0, 0, false, true} : Rule{true, g % m_width, g / m_width, second};
            };
            m_emptyEarly[i] = unlitFrom ? own : unlitTo ? ownTo : near(m_from, darkFrom, m_backgroundFrom, false);
            m_emptyLate[i] = unlitTo ? ownTo : unlitFrom ? own : near(m_to, darkTo, m_backgroundTo, true);
         }
   }

   // `sweep` false: random moments. Without the other frame (shades empty) there is nothing to do.
   Morph(bool sweep, Direction direction, const SourceInfo& s)
      : PixelEffect(s.width, s.height), m_from(s.shades), m_to(s.shadesSecond), m_backgroundFrom(s.background),
        m_backgroundTo(s.backgroundSecond)
   {
      const uint32_t width = s.width, height = s.height;
      const size_t cells = static_cast<size_t>(width) * height;
      makeEmpties();
      std::vector<uint32_t> a = litPlaces(m_from, width, height);
      std::vector<uint32_t> b = litPlaces(m_to, width, height);
      if (a.empty() || b.empty())
         return;
      // What is the same in both frames stays where it is; the moving pixels pass in front of it.
      m_static.assign(cells, 0);
      if (s.same.size() == cells)
      {
         const uint8_t darkest = *std::min_element(m_from.begin(), m_from.end());
         for (size_t i = 0; i < cells; ++i)
            m_static[i] = s.same[i] && m_from[i] > darkest ? 1 : 0;
         auto moving = [&](uint32_t i) { return !m_static[i]; };
         a.erase(std::remove_if(a.begin(), a.end(), [&](uint32_t i) { return !moving(i); }), a.end());
         b.erase(std::remove_if(b.begin(), b.end(), [&](uint32_t i) { return !moving(i); }), b.end());
         if (a.empty() || b.empty())
            return;
      }
      // Pixels go to pixels of the same color: white to white (and there are more of them if the other
      // picture has more white). Colors only one frame has, and what is left, are paired by place.
      auto tone = [&](const std::vector<uint16_t>& tones, const std::vector<uint8_t>& shades, uint32_t i) {
         return tones.size() == cells ? tones[i] : static_cast<uint16_t>(shades[i]);
      };
      std::map<uint16_t, std::pair<std::vector<uint32_t>, std::vector<uint32_t>>> groups;
      for (uint32_t i : a)
         groups[tone(s.tones, m_from, i)].first.push_back(i);
      for (uint32_t i : b)
         groups[tone(s.tonesSecond, m_to, i)].second.push_back(i);
      std::vector<Pair> pairs;
      std::vector<uint32_t> restA, restB;
      for (const auto& g : groups)
      {
         if (!g.second.first.empty() && !g.second.second.empty())
            match(g.second.first, g.second.second, width, pairs);
         else
         {
            restA.insert(restA.end(), g.second.first.begin(), g.second.first.end());
            restB.insert(restB.end(), g.second.second.begin(), g.second.second.end());
         }
      }
      if (!restA.empty() && !restB.empty())
         match(restA, restB, width, pairs);
      else if (!restA.empty())
         match(restA, spread(b, restA.size()), width, pairs);
      else if (!restB.empty())
         match(spread(a, restB.size()), restB, width, pairs);
      const size_t count = pairs.size();
      std::vector<Pt> pa(count), pb(count);
      for (size_t i = 0; i < count; ++i)
      {
         pa[i] = pairs[i].from;
         pb[i] = pairs[i].to;
      }
      m_parts.resize(count);
      for (size_t i = 0; i < count; ++i)
      {
         Part& p = m_parts[i];
         p.sx = pa[i].x;
         p.sy = pa[i].y;
         p.dx = pb[i].x;
         p.dy = pb[i].y;
         const double r = unitOf(mix64(0x30F4ull * 0x10001ull + i), 0);
         if (sweep)
         {
            p.delay = 0.25 * sweepPosition(direction, p.sx, p.sy, width, height) + 0.08 * r;
            p.span = 0.67;
         }
         else
         {
            p.delay = 0.30 * r;
            p.span = 0.70;
         }
      }
      // The first pixel sets off at once, and the last one arrives with the last frame.
      double first = 1e9;
      for (const Part& p : m_parts)
         first = std::min(first, p.delay);
      for (Part& p : m_parts)
         p.delay -= first;
      // The time runs from the first change anyone makes (a pixel moves a whole pixel, or changes its
      // color) to the last one, so that pictures that are alike (little to move, only colors to
      // change) still have a different picture at every step.
      m_first = 1e9;
      m_end = 0;
      for (const Part& p : m_parts)
      {
         const double distance = std::max(std::abs(static_cast<double>(p.dx) - p.sx), std::abs(static_cast<double>(p.dy) - p.sy));
         const double flip = p.delay + 0.5 * p.span;
         m_first = std::min(m_first, flip);
         m_end = std::max(m_end, flip);
         if (distance >= 1.0)
         {
            // The local times at which it has moved half a pixel, and at which it is within half a
            // pixel of the end (the eased progress only grows).
            double low = 0, high = 1;
            for (int i = 0; i < 40; ++i)
            {
               const double mid = 0.5 * (low + high);
               (progress(mid) * distance >= 0.5 ? high : low) = mid;
            }
            m_first = std::min(m_first, p.delay + p.span * high);
            low = 0;
            high = 1;
            for (int i = 0; i < 40; ++i)
            {
               const double mid = 0.5 * (low + high);
               ((1.0 - progress(mid)) * distance < 0.5 ? high : low) = mid;
            }
            m_end = std::max(m_end, p.delay + p.span * high);
         }
      }
      if (m_end <= m_first)
         m_end = m_first + 0.01;
   }
   const char* limitReason() const override
   {
      return ready() ? "the pixels travel a few pixels more with every frame"
                     : "Morph goes to another frame, and both frames need lit pixels";
   }

protected:
   bool ready() const override { return !m_parts.empty(); }
   void build(std::vector<Rule>& cells, uint32_t step, uint32_t steps) const override
   {
      // What a place shows when no pixel is on it: the background of the first frame, and from the
      // halfway point that of the other one; black when a frame has no background.
      const bool late = 2 * step >= steps;
      const std::vector<Rule>& empty = late ? m_emptyLate : m_emptyEarly;
      for (size_t i = 0; i < cells.size(); ++i)
         cells[i] = i < empty.size() ? empty[i] : Rule{false, 0, 0, false, true};
      std::vector<int> rank(cells.size(), -1);
      for (size_t i = 0; i < m_static.size() && i < cells.size(); ++i)
         if (m_static[i])
         {
            cells[i] = Rule{true, static_cast<uint32_t>(i % m_width), static_cast<uint32_t>(i / m_width)};
            rank[i] = 0; // the pixels on their way pass in front
         }
      const double t = static_cast<double>(step) / steps;
      for (const Part& p : m_parts)
      {
         // The time runs until the last pixel has arrived, so that the last frame is the picture.
         const double local = std::min(1.0, std::max(0.0, (m_first + (m_end - m_first) * t - p.delay) / p.span));
         const double e = progress(local);
         const int32_t px = rounded(p.sx + (static_cast<double>(p.dx) - p.sx) * e);
         const int32_t py = rounded(p.sy + (static_cast<double>(p.dy) - p.sy) * e);
         const bool arrived = local >= 0.5; // the pixel has its new color from the halfway point
         const size_t from = static_cast<size_t>(p.sy) * m_width + p.sx;
         const size_t to = static_cast<size_t>(p.dy) * m_width + p.dx;
         const int level = arrived ? m_to[to] : m_from[from];
         const size_t at = static_cast<size_t>(py) * m_width + px;
         if (level > rank[at])
         {
            rank[at] = level;
            cells[at] = arrived ? Rule{true, p.dx, p.dy, true} : Rule{true, p.sx, p.sy};
         }
      }
   }

private:
   struct Pt
   {
      uint32_t x, y;
   };
   struct Part
   {
      uint32_t sx = 0, sy = 0, dx = 0, dy = 0;
      double delay = 0, span = 1;
   };
   // Pairs a[lo, hi) with b[lo, hi) (equally long): cut both along the longer side of the box that
   // holds them, in the middle, and pair the halves. In the end the pixel i of a goes to pixel i of b.
   struct Pair
   {
      Pt from, to;
   };
   // `count` of the places, taken evenly (some more than once if there are fewer).
   static std::vector<uint32_t> spread(const std::vector<uint32_t>& places, size_t count)
   {
      std::vector<uint32_t> out(count);
      for (size_t i = 0; i < count; ++i)
         out[i] = places[i * places.size() / count];
      return out;
   }
   // Pairs two sets of places (row * width + column) so that the pixels hardly cross: as many pairs
   // as the larger set has pixels, so the smaller set's pixels are used more than once.
   static void match(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b, uint32_t width,
                     std::vector<Pair>& out)
   {
      const size_t count = std::max(a.size(), b.size());
      std::vector<Pt> pa(count), pb(count);
      for (size_t i = 0; i < count; ++i)
      {
         const uint32_t ia = a[i * a.size() / count], ib = b[i * b.size() / count];
         pa[i] = {ia % width, ia / width};
         pb[i] = {ib % width, ib / width};
      }
      pair(pa, pb, 0, count);
      for (size_t i = 0; i < count; ++i)
         out.push_back({pa[i], pb[i]});
   }
   // How far a pixel is on its way (0 .. 1) at a local time (0 .. 1): slow at the start and the end.
   static double progress(double local)
   {
      return local >= 1.0 ? 1.0 : 0.5 * (local + local * local * (3.0 - 2.0 * local));
   }
   static void pair(std::vector<Pt>& a, std::vector<Pt>& b, size_t lo, size_t hi)
   {
      if (hi - lo < 2)
         return;
      uint32_t x0 = UINT32_MAX, x1 = 0, y0 = UINT32_MAX, y1 = 0;
      for (const std::vector<Pt>* v : {&a, &b})
         for (size_t i = lo; i < hi; ++i)
         {
            x0 = std::min(x0, (*v)[i].x);
            x1 = std::max(x1, (*v)[i].x);
            y0 = std::min(y0, (*v)[i].y);
            y1 = std::max(y1, (*v)[i].y);
         }
      const bool byX = x1 - x0 >= y1 - y0;
      auto less = [byX](const Pt& l, const Pt& r) {
         return byX ? (l.x != r.x ? l.x < r.x : l.y < r.y) : (l.y != r.y ? l.y < r.y : l.x < r.x);
      };
      std::sort(a.begin() + lo, a.begin() + hi, less);
      std::sort(b.begin() + lo, b.begin() + hi, less);
      const size_t mid = lo + (hi - lo) / 2;
      pair(a, b, lo, mid);
      pair(a, b, mid, hi);
   }

   std::vector<uint8_t> m_from, m_to;
   uint32_t m_backgroundFrom, m_backgroundTo;
   std::vector<uint8_t> m_static; // places that are the same in both frames and stay
   std::vector<Rule> m_emptyEarly, m_emptyLate; // what a place without a pixel shows (see makeEmpties)
   double m_first = 0, m_end = 0;
   std::vector<Part> m_parts;
};

// ---- The list -------------------------------------------------------------------------------

// A choice a transition has (its direction, its type ...).
struct Option
{
   const char* id;    // kept in the plugin's settings
   const char* label; // shown in the window
};

struct Transition
{
   const char* id;           // kept in the plugin's settings
   const char* label;        // shown in the window
   const char* description;  // a sentence or two for the window
   const char* optionLabel;  // what the options are called ("Direction"); null without options
   const Option* options;
   size_t optionCount;
   std::unique_ptr<Effect> (*make)(const char* optionId, const SourceInfo& source);
   bool canMoveSecond = false; // in a transition to another frame, that frame can come in with the effect
   bool needsFrame = false;    // the transition only works when it goes to another frame
};

inline bool same(const char* a, const char* b) { return std::string_view(a) == std::string_view(b); }

// A transition to another frame whose other frame comes in with the effect.
inline bool follows(const SourceInfo& s) { return s.toFrame && s.secondMoves; }

inline std::unique_ptr<Effect> makeDissolve(const char* option, const SourceInfo& s)
{
   if (same(option, "random")) return std::make_unique<RankDissolve>(Order::Random, s.width, s.height);
   if (same(option, "wpc")) return std::make_unique<WpcDissolve>();
   return std::make_unique<OrderlyDissolve>(s.width, s.height);
}

inline Direction directionOf(const char* option)
{
   if (same(option, "up")) return Direction::Up;
   if (same(option, "down")) return Direction::Down;
   if (same(option, "right")) return Direction::Right;
   return Direction::Left;
}

inline std::unique_ptr<Effect> makeSlide(const char* option, const SourceInfo& s)
{
   return std::make_unique<Slide>(directionOf(option), s.width, s.height, follows(s));
}
inline std::unique_ptr<Effect> makeWipe(const char* option, const SourceInfo& s)
{
   // A wipe is a bar that moves across the picture, or a shape that grows from the centre, closes
   // in from the edges or winds along a spiral.
   if (same(option, "centre_out")) return std::make_unique<RankDissolve>(Order::CentreOut, s.width, s.height);
   if (same(option, "edge_in")) return std::make_unique<RankDissolve>(Order::EdgeIn, s.width, s.height);
   if (same(option, "spiral")) return std::make_unique<RankDissolve>(Order::Spiral, s.width, s.height);
   return std::make_unique<Wipe>(directionOf(option), s.width, s.height);
}
inline std::unique_ptr<Effect> makeBlinds(const char* option, const SourceInfo& s)
{
   return std::make_unique<Blinds>(!same(option, "vertical"), s.width, s.height);
}
inline std::unique_ptr<Effect> makeCheckerboard(const char* option, const SourceInfo& s)
{
   const uint32_t size = same(option, "16") ? 16u : same(option, "8") ? 8u : same(option, "4") ? 4u : 2u;
   return std::make_unique<Checkerboard>(size, s.width, s.height);
}
inline std::unique_ptr<Effect> makePixelate(const char*, const SourceInfo& s)
{
   return std::make_unique<Pixelate>(s.width, s.height, s.toFrame);
}
inline std::unique_ptr<Effect> makeWave(const char* option, const SourceInfo& s)
{
   return std::make_unique<Curtain>(!same(option, "vertical"), Curtain::Speeds::Wave, s.width, s.height, follows(s));
}
// Melt: the columns drip down, in a wave of speeds (Smooth) or at random-looking speeds (Dripping).
inline std::unique_ptr<Effect> makeMelt(const char* option, const SourceInfo& s)
{
   return std::make_unique<Curtain>(false, same(option, "drips") ? Curtain::Speeds::Drips : Curtain::Speeds::Wave,
                                    s.width, s.height, follows(s));
}
inline std::unique_ptr<Effect> makeZoom(const char* option, const SourceInfo& s)
{
   if (same(option, "out"))
      return std::make_unique<ZoomOut>(s.width, s.height);
   return std::make_unique<ZoomIn>(s.width, s.height, s.toFrame);
}
inline std::unique_ptr<Effect> makeFade(const char*, const SourceInfo& s)
{
   return std::make_unique<Fade>(s.toWhite, s);
}
inline std::unique_ptr<Effect> makeAlternatingSlide(const char* option, const SourceInfo& s)
{
   return std::make_unique<AlternatingSlide>(!same(option, "vertical"), s.width, s.height, follows(s));
}
// Split: Vertical cuts down the middle (the halves move left and right), Horizontal cuts across
// the middle (the halves move up and down).
inline std::unique_ptr<Effect> makeSplit(const char* option, const SourceInfo& s)
{
   return std::make_unique<Split>(same(option, "vertical"), s.width, s.height);
}

// Dust: the direction the wind blows.
inline std::unique_ptr<Effect> makeDust(const char* option, const SourceInfo& s)
{
   const Direction wind = same(option, "left") ? Direction::Left : same(option, "up") ? Direction::Up
                          : same(option, "down") ? Direction::Down : Direction::Right;
   return std::make_unique<Dust>(wind, s.shades, s.width, s.height);
}
// Morph: the pixels leave at random moments or in a sweep; without another frame there is nothing
// to morph into (the effect then offers no frames).
inline std::unique_ptr<Effect> makeMorph(const char* option, const SourceInfo& s)
{
   const bool sweep = !same(option, "random");
   const Direction d = same(option, "left") ? Direction::Left : same(option, "up") ? Direction::Up
                       : same(option, "down") ? Direction::Down : Direction::Right;
   if (!s.toFrame)
   {
      SourceInfo nothing;
      nothing.width = s.width;
      nothing.height = s.height;
      return std::make_unique<Morph>(sweep, d, nothing);
   }
   return std::make_unique<Morph>(sweep, d, s);
}

inline const Option kDissolveOptions[] = {
   {"orderly", "Orderly"}, {"random", "Random"}, {"wpc", "WPC"},
};
inline const Option kDirectionOptions[] = {
   {"up", "Up"}, {"down", "Down"}, {"left", "Left"}, {"right", "Right"},
};
inline const Option kOrientationOptions[] = {
   {"horizontal", "Horizontal"}, {"vertical", "Vertical"},
};
inline const Option kSquareOptions[] = {
   {"2", "2 x 2"}, {"4", "4 x 4"}, {"8", "8 x 8"}, {"16", "16 x 16"},
};
inline const Option kMeltOptions[] = {
   {"smooth", "Smooth"}, {"drips", "Dripping"},
};
inline const Option kWindOptions[] = {
   {"right", "Right"}, {"left", "Left"}, {"up", "Up"}, {"down", "Down"},
};
inline const Option kMorphOptions[] = {
   {"random", "Random"}, {"right", "Sweep Right"}, {"left", "Sweep Left"}, {"down", "Sweep Down"}, {"up", "Sweep Up"},
};
inline const Option kZoomOptions[] = {
   {"in", "In"}, {"out", "Out"},
};
inline const Option kWipeOptions[] = {
   {"up", "Up"}, {"down", "Down"}, {"left", "Left"}, {"right", "Right"},
   {"centre_out", "Centre-Out"}, {"edge_in", "Edge-In"}, {"spiral", "Spiral"},
};

#define TB_COUNT(a) (sizeof(a) / sizeof((a)[0]))

inline const Transition kTransitions[] = {
   {"alternating_slide", "Alternating Slide",
    "Horizontal: even rows (0,2,4) slide left and odd rows (1,3,5) slide right\n"
    "Vertical: even columns (0,2,4) slide up and odd columns (1,3,5) slide down",
    "Orientation", kOrientationOptions, TB_COUNT(kOrientationOptions), &makeAlternatingSlide, true},
   {"blinds", "Blinds",
    "Bars appear at regular intervals and grow wider.\n"
    "Horizontal: horizontal bars, stacked.\n"
    "Vertical: vertical bars, side by side.",
    "Orientation", kOrientationOptions, TB_COUNT(kOrientationOptions), &makeBlinds},
   {"checkerboard", "Checkerboard",
    "The picture is cut into squares that disappear one by one: first the squares of one color "
    "of a checkerboard, in a sweep from the top left corner, then the others.",
    "Square Size", kSquareOptions, TB_COUNT(kSquareOptions), &makeCheckerboard},
   {"dissolve", "Dissolve",
    "The picture dissolves pixel by pixel.\n"
    "Orderly: pixels disappear in a regular pattern.\n"
    "Random: pixels disappear at random.\n"
    "WPC: the dot pattern of the Bally/Williams games, an eighth of the pixels at a time.",
    "Dissolve Type", kDissolveOptions, TB_COUNT(kDissolveOptions), &makeDissolve},
   {"dust", "Dust",
    "The picture crumbles to dust and the wind blows it away.\n"
    "The option is the direction the wind blows. The pixels fly off in a wave, starting on the side the wind comes from.",
    "Wind", kWindOptions, TB_COUNT(kWindOptions), &makeDust},
   {"fade", "Fade",
    "The first frame gradually fades to black, white or the second frame.",
    nullptr, nullptr, 0, &makeFade},
   {"melt", "Melt",
    "The picture melts down off the screen: every column slides down at its own speed.\n"
    "Smooth: the speeds follow a wave.\n"
    "Dripping: the speeds look random, like paint dripping.",
    "Style", kMeltOptions, TB_COUNT(kMeltOptions), &makeMelt, true},
   {"morph", "Morph",
    "The lit pixels of the first frame travel across the screen and become the lit pixels of the second frame. "
    "Everything else is black. Needs Another frame.\n"
    "Random: the pixels set off at random moments.\n"
    "Sweep: they set off in a wave, starting on the side you choose.",
    "Order", kMorphOptions, TB_COUNT(kMorphOptions), &makeMorph, false, true},
   {"pixelate", "Pixelate",
    "The first frame turns into bigger and bigger blocks, slowly at first and then faster.\n"
    "To another frame: the other frame then comes out of the blocks and sharpens.",
    nullptr, nullptr, 0, &makePixelate},
   {"slide", "Slide",
    "The whole picture slides off the screen in the direction you choose.",
    "Direction", kDirectionOptions, TB_COUNT(kDirectionOptions), &makeSlide, true},
   {"split", "Split",
    "The picture is cut in the middle and the two halves slide apart.\n"
    "Horizontal: cut across, the top half slides up and the bottom half down.\n"
    "Vertical: cut down, the left half slides left and the right half right.",
    "Orientation", kOrientationOptions, TB_COUNT(kOrientationOptions), &makeSplit},
   {"wave", "Wave",
    "The picture turns into a wavy curtain that is pulled out of the frame. The crests go twice as fast as the troughs.\n"
    "Horizontal: the rows slide sideways, in one wave over the height.\n"
    "Vertical: the columns slide down, in four waves over the width.",
    "Orientation", kOrientationOptions, TB_COUNT(kOrientationOptions), &makeWave, true},
   {"wipe", "Wipe",
    "A bar wipes over the picture in the direction you choose\n"
    "Left: it comes in from the right edge and moves left.\n"
    "Centre-Out: an oval shape grows from the centre.\n"
    "Edge-In: the picture closes in from the outside edges.\n"
    "Spiral: a spiral winds in from the top left corner.",
    "Wipe Type", kWipeOptions, TB_COUNT(kWipeOptions), &makeWipe},
   {"zoom", "Zoom",
    "Zoom In: the picture grows toward you, a smaller and smaller part of the middle fills the frame.\n"
    "To another frame: it zooms into the first frame, then the other frame zooms out to full size.\n"
    "Zoom Out: the picture shrinks toward the middle.",
    "Zoom", kZoomOptions, TB_COUNT(kZoomOptions), &makeZoom},
};
inline constexpr size_t kTransitionCount = 14;
static_assert(sizeof(kTransitions) / sizeof(kTransitions[0]) == kTransitionCount, "update kTransitionCount");

#undef TB_COUNT

inline const Transition* findTransition(const char* id)
{
   for (const Transition& transition : kTransitions)
      if (same(id, transition.id))
         return &transition;
   return nullptr;
}

// The option of a transition by id; the first one for an id it does not know; null without any.
inline const Option* findOption(const Transition& transition, const char* id)
{
   if (transition.optionCount == 0)
      return nullptr;
   for (size_t i = 0; i < transition.optionCount; ++i)
      if (same(id, transition.options[i].id))
         return &transition.options[i];
   return &transition.options[0];
}

} // namespace transitions
