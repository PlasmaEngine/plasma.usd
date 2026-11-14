-- Plasma USD Live, before 3ds Max's UI starts: the macroScripts behind the Plasma menu, and in 3ds Max 2025 and later
-- the menu itself, which the menu system asks for with #cuiRegisterMenus once it loads. 3ds Max 2024 builds its menus
-- with menuMan, which plasma_usd_live.ms does after startup.
-- The macroScripts run Python only when used, so nothing here needs the connector's Python folder yet.

macroScript UsdLive category:"Plasma" buttonText:"USD Live..." toolTip:"Join the Plasma editor's USD live sessions"
(
    python.Execute "import plasma_usd_live.panel\nplasma_usd_live.panel.open_panel()"
)

macroScript UsdLiveLeave category:"Plasma" buttonText:"Leave USD Live Session" toolTip:"Leave the USD live session"
(
    python.Execute "import plasma_usd_live\nplasma_usd_live.leave()"
)

if (maxVersion())[1] >= 27000 do
(
    global plasmaUsdLive_registerMenus
    fn plasmaUsdLive_registerMenus =
    (
        try
        (
            local menuManager = callbacks.notificationParam()
            local mainMenuBar = menuManager.mainMenuBar
            -- 647394 is the action table of macroScripts; their actions are named "<macroScript>`<category>".
            local plasmaMenu = mainMenuBar.CreateSubMenu "5b0d9a3e-7c41-4f2e-9a61-2d8f3c6e1a01" "Plasma"
            plasmaMenu.CreateAction "5b0d9a3e-7c41-4f2e-9a61-2d8f3c6e1a02" 647394 "UsdLive`Plasma"
            plasmaMenu.CreateAction "5b0d9a3e-7c41-4f2e-9a61-2d8f3c6e1a03" 647394 "UsdLiveLeave`Plasma"
        )
        catch
        (
            format "[Plasma USD Live] Could not add the Plasma menu: %\n" (getCurrentException())
        )
    )
    callbacks.removeScripts id:#plasmaUsdLiveMenus
    callbacks.addScript #cuiRegisterMenus plasmaUsdLive_registerMenus id:#plasmaUsdLiveMenus
)
