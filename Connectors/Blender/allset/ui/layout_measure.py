# SPDX-License-Identifier: GPL-3.0-or-later
"""How tall a draw function's UI is, in rows (issue #4: torn-off windows sized to their content).

Blender has no API for the height a layout will take, so ``rows(draw, context)`` runs the draw function on a
stand-in layout that records the structure: a column / box stacks its children, a row / split is as tall as its
tallest child, every button / label is one row (``scale_y`` and ``ui_units_y`` respected), a separator a fraction
of one, a list its row count. Anything the stand-in doesn't know is one button. A row is ``UI_UNIT_Y`` = 20 px x
the effective UI scale.
"""

ROW_PX = 20
LAST_ERROR = []
SEPARATOR = 0.35
BOX_PAD = 0.5
_COLUMN_KINDS = {"column": "COL", "box": "BOX", "grid_flow": "COL", "column_flow": "COL", "menu_pie": "COL"}
_ROW_KINDS = {"row": "ROW", "split": "ROW"}


class _Props:
    """What ``layout.operator`` / ``prop`` return: accepts any attribute."""

    def __setattr__(self, name, value):
        object.__setattr__(self, name, value)

    def __getattr__(self, name):
        return None


class Layout:
    """A stand-in UILayout that records rows."""

    def __init__(self, kind="COL"):
        object.__setattr__(self, "_kind", kind)
        object.__setattr__(self, "_children", [])
        object.__setattr__(self, "scale_y", 1.0)
        object.__setattr__(self, "ui_units_y", 0.0)

    def __setattr__(self, name, value):
        object.__setattr__(self, name, value)

    def _sub(self, kind):
        child = Layout(kind)
        child.scale_y = self.scale_y
        self._children.append(child)
        return child

    def _leaf(self, rows=1.0):
        self._children.append(rows * self.scale_y)
        return _Props()

    def __getattr__(self, name):
        if name.startswith("__"):
            raise AttributeError(name)
        if name in _COLUMN_KINDS:
            return lambda *a, **k: self._sub(_COLUMN_KINDS[name])
        if name in _ROW_KINDS:
            return lambda *a, **k: self._sub(_ROW_KINDS[name])
        if name in ("separator", "separator_spacer"):
            return lambda *a, factor=1.0, **k: self._leaf(SEPARATOR * factor if name == "separator" else 0.0)
        if name in ("panel", "panel_prop"):
            # (header, body): the body exists when the panel is open; its stored open state isn't readable here,
            # so the default state stands in for it
            def panel(*_a, default_closed=False, **_k):
                return self._sub("ROW"), (None if default_closed else self._sub("COL"))
            return panel
        if name == "template_list":
            return lambda *a, rows=5, **k: self._leaf(rows + 1)
        if name in ("context_pointer_set", "context_string_set"):
            return lambda *a, **k: None
        return lambda *a, **k: self._leaf()

    def height(self):
        """Rows this layout takes."""
        heights = [c.height() if isinstance(c, Layout) else c for c in self._children]
        if self._kind == "ROW":
            h = max(heights, default=0.0)
        else:
            h = sum(heights)
            if self._kind == "BOX" and heights:
                h += BOX_PAD
        return max(h, self.ui_units_y or 0.0)


class _Holder:
    def __init__(self, layout):
        self.layout = layout


def rows(draw, context, *args):
    """Rows ``draw(holder, context)`` lays out (a Panel / Menu draw method), or None when it fails here."""
    lay = Layout()
    try:
        draw(_Holder(lay), context, *args)
    except Exception as ex:    # a draw that needs a real layout (or the real region): no measure
        LAST_ERROR[:] = [repr(ex)]
        return None
    return lay.height()


def px(n_rows, context):
    """Pixels of ``n_rows`` UI rows at the effective UI scale."""
    s = getattr(context.preferences.system, "ui_scale", 1.0) or 1.0
    return n_rows * ROW_PX * s
