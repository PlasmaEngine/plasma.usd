"""Qt for the connector: PySide6 in 3ds Max 2025 and later, PySide2 in 3ds Max 2024."""

try:
    from PySide6 import QtCore, QtGui, QtWidgets  # noqa: F401
    import shiboken6 as shiboken
except ImportError:
    from PySide2 import QtCore, QtGui, QtWidgets  # noqa: F401
    import shiboken2 as shiboken


def exec_dialog(dialog):
    """Runs a modal dialog: exec in PySide6, exec_ in PySide2."""
    run = getattr(dialog, "exec", None) or dialog.exec_
    return run()


def is_valid(widget):
    """False once Qt has deleted the widget."""
    try:
        return widget is not None and shiboken.isValid(widget)
    except Exception:
        return False


def main_window():
    """3ds Max's main window, the parent of the panel and its dialogs."""
    try:
        from qtmax import GetQMaxMainWindow
        return GetQMaxMainWindow()
    except ImportError:
        return None
