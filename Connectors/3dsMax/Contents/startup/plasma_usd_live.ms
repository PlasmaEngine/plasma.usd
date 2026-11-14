-- Plasma USD Live, once 3ds Max's UI is up: puts the connector's Python folder (../python beside this script) on
-- Python's path, adds the Plasma menu in 3ds Max 2024 (later versions get it from plasma_usd_live_menu.ms), and joins
-- the session when the Plasma editor launched this 3ds Max into one (PLASMA_USD_LIVE_STAGE and PLASMA_USD_LIVE_SESSION).
(
    local pythonFolder = pathConfig.normalizePath ((getFilenamePath (getSourceFileName())) + "..\\python")
    python.Execute ("import sys\nfolder = r'" + pythonFolder + "'\nif folder not in sys.path:\n    sys.path.insert(0, folder)")

    if (maxVersion())[1] < 27000 do
    (
        try
        (
            local plasmaMenu = menuMan.findMenu "Plasma"
            if plasmaMenu == undefined do
            (
                plasmaMenu = menuMan.createMenu "Plasma"
                local mainMenuBar = menuMan.getMainMenuBar()
                mainMenuBar.addItem (menuMan.createSubMenuItem "Plasma" plasmaMenu) -1
            )
            for macro in #("UsdLive", "UsdLiveLeave") do
            (
                local item = menuMan.createActionItem macro "Plasma"
                local known = false
                for i = 1 to plasmaMenu.numItems() do
                    if (plasmaMenu.getItem i).getTitle() == item.getTitle() do known = true
                if not known do plasmaMenu.addItem item -1
            )
            menuMan.updateMenuBar()
        )
        catch
        (
            format "[Plasma USD Live] Could not add the Plasma menu: %\n" (getCurrentException())
        )
    )

    python.Execute "import plasma_usd_live.panel\ntry:\n    plasma_usd_live.panel.join_from_environment()\nexcept Exception:\n    import traceback\n    traceback.print_exc()"
)
