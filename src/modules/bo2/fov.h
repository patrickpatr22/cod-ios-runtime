// BO2 iOS - field of view. The game's cg_fov (65 degrees across a 4:3
// picture) is reset by the game scripts, so instead the angle the view is
// drawn with is scaled where the game computes it.

#pragma once

namespace bo2 {

constexpr int kFovGame = 65;
constexpr int kFovMax = 110;

// Degrees as the game counts cg_fov (65 = as on the console). Applies from
// the next frame.
void SetFieldOfView(int degrees);

// "iPad full screen" picture: the game runs in its 4:3 mode and its picture
// is shown over the whole display (width / height = display_aspect). The 3D
// view keeps the width of the 16:9 view and shows more above and below
// (vertical FOV grows), so nothing is stretched. 0 turns it off.
void SetVertPlusAspect(double display_aspect);

}  // namespace bo2
