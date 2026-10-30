# SPDX-License-Identifier: GPL-3.0-or-later
"""Maya 2026 Auto tangents (step 257, Issue #151).

Maya's factory in / out tangent for new keys is ``auto`` (non-weighted). Its slope rule
(Maya 2026 behaviour, 15 curves):

1. the first and the last key are flat;
2. a middle key takes the central difference m = (v+ - v-) / (t+ - t-) over both neighbours;
3. m = 0 when the two secants s-, s+ differ in sign or m's sign differs from theirs (a zero secant counts as
   positive), else |m| <= min(3|s-|, 3|s+|);
4. the in / out tangents are (h-, h- m) / (h+, h+ m): a plain cubic Hermite in time = Bezier handles at a
   third of each interval.

Blender has one automatic handle type with this meaning ('AUTO_CLAMPED', the kit's Maya Auto since step 56),
but places its handles by its own rule, so the kit writes Maya's handles on such keys after it sets a key,
removes one or picks the Auto tangent. Maya recomputes auto tangents when a neighbour key changes; Blender's
own edits (a Graph Editor drag) recompute Blender's shape until the kit touches the curve again.
"""

AUTO = 'AUTO_CLAMPED'


def _sign(x):
    return -1 if x < 0 else 1           # Maya counts a zero secant as positive


def slopes(times, values):
    """Maya auto slope (value per frame) of every key of a curve whose keys are (times, values), times rising."""
    n = len(times)
    out = [0.0] * n
    for k in range(1, n - 1):
        h0, h1 = times[k] - times[k - 1], times[k + 1] - times[k]
        if h0 <= 0 or h1 <= 0:
            continue
        s0 = (values[k] - values[k - 1]) / h0
        s1 = (values[k + 1] - values[k]) / h1
        m = (values[k + 1] - values[k - 1]) / (h0 + h1)
        if _sign(s0) != _sign(s1) or _sign(m) != _sign(s0):
            continue
        lim = min(3.0 * abs(s0), 3.0 * abs(s1))
        out[k] = max(-lim, min(lim, m))
    return out


def handles(times, values):
    """[(left x, left y, right x, right y)] Bezier handles of the Maya auto curve through the keys."""
    m = slopes(times, values)
    n = len(times)
    out = []
    for k in range(n):
        h0 = (times[k] - times[k - 1]) if k > 0 else None
        h1 = (times[k + 1] - times[k]) if k < n - 1 else None
        h0 = h0 if h0 is not None else (h1 if h1 is not None else 3.0)
        h1 = h1 if h1 is not None else h0
        t, v = times[k], values[k]
        out.append((t - h0 / 3.0, v - m[k] * h0 / 3.0, t + h1 / 3.0, v + m[k] * h1 / 3.0))
    return out


def is_auto(kp):
    return kp.handle_left_type == AUTO and kp.handle_right_type == AUTO


def apply(fc):
    """Write Maya's auto handles on the curve's Auto keys. Returns how many keys got them."""
    pts = fc.keyframe_points
    if not len(pts):
        return 0
    times = [kp.co.x for kp in pts]
    if any(b < a for a, b in zip(times, times[1:])):
        fc.update()                     # sorts the keys
        times = [kp.co.x for kp in pts]
    values = [kp.co.y for kp in pts]
    n = 0
    for kp, (lx, ly, rx, ry) in zip(pts, handles(times, values)):
        if not is_auto(kp):
            continue
        kp.handle_left = (lx, ly)
        kp.handle_right = (rx, ry)
        n += 1
    return n


def apply_all(fcurves):
    return sum(apply(fc) for fc in fcurves)
