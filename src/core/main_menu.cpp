#include "main_menu.h"
#include "display.h"
#include "module_probe.h"
#include "mq_features.h"
#include "utils.h"
#include <globals.h>

namespace {
class ModulesMenuItem : public MenuItemInterface {
public:
    ModulesMenuItem() : MenuItemInterface("Modules") {}
    void optionsMenu(void) override { showModuleStatus(); }
    bool hasTheme() override { return false; }
    const String &themePath() override {
        static const String empty;
        return empty;
    }
    void drawIcon(float scale = 1) override {
        clearIconArea();
        int w = max(16, (int)(44 * scale));
        int h = max(14, (int)(32 * scale));
        int x = iconCenterX - w / 2;
        int y = iconCenterY - h / 2;
        int r = max(2, (int)(4 * scale));
        tft.drawRoundRect(x, y, w, h, r, bruceConfig.priColor);
        int barW = max(4, (w - 10) / 3);
        int barH = max(6, h - 10);
        int gap = max(2, (w - 6 - barW * 3) / 2);
        int bx = x + (w - (barW * 3 + gap * 2)) / 2;
        int by = y + (h - barH) / 2;
        for (int i = 0; i < 3; i++) {
            tft.fillRoundRect(bx + i * (barW + gap), by, barW, barH, 1, bruceConfig.priColor);
        }
    }
};

ModulesMenuItem mqModulesMenuItem;
} // namespace

MainMenu::MainMenu() {
    _menuItems = {
        &wifiMenu,
        &bleMenu,
        &rfMenu,
        &nrf24Menu,
#if !defined(LITE_VERSION)
        &loraMenu,
#endif
#if !defined(LITE_VERSION)
        &fmMenu,
#endif
        &irMenu,
#if !defined(LITE_VERSION)
        &ethernetMenu,
#endif
        &gpsMenu,
        &rfidMenu,
        &fileMenu,
#if !defined(LITE_VERSION) && !defined(DISABLE_INTERPRETER)
        &scriptsMenu,
#endif
        &clockMenu,
        &othersMenu,
#if !defined(LITE_VERSION)
        &wireToolsMenu,
#endif
        &configMenu,
    };

    _totalItems = _menuItems.size();

#if defined(HAS_TOUCH)
    gridPageTapHandler = [](int x, int y, int currentIndex, int &newIndex) -> bool {
        return mainMenu.handleGridPageTap(x, y, currentIndex, newIndex);
    };
#endif
}

MainMenu::~MainMenu() {}

void MainMenu::begin(void) {
    returnToMenu = false;
    options = {};
    probeAllModules(false);

    bool gridStyle = bruceConfig.mainMenuStyle == MAIN_MENU_GRID;
    bool listStyle = bruceConfig.mainMenuStyle == MAIN_MENU_LIST;

    // Option::hover is a plain function pointer, so both renderers stay capture-less
    // and the grid/list ones resolve their own position from the item pointer they receive.
    bool (*renderItem)(void *menuItem, bool shouldRender) = [](void *menuItem, bool shouldRender) {
        if (!shouldRender) return false;
        drawMainBorder(false);

        MenuItemInterface *obj = static_cast<MenuItemInterface *>(menuItem);
        float scale = float((float)tftWidth / (float)240);
        if (bruceConfigPins.rotation & 0b01) scale = float((float)tftHeight / (float)135);
        obj->draw(scale);
#if defined(HAS_TOUCH)
        TouchFooter();
#endif
        return true;
    };

    if (gridStyle) {
        renderItem = [](void *menuItem, bool shouldRender) {
            if (!shouldRender) return false;
            mainMenu.drawGrid(mainMenu.gridIndexOf(static_cast<MenuItemInterface *>(menuItem)));
            return true;
        };
    } else if (listStyle) {
        renderItem = [](void *menuItem, bool shouldRender) {
            if (!shouldRender) return false;
            mainMenu.drawList(mainMenu.gridIndexOf(static_cast<MenuItemInterface *>(menuItem)));
            return true;
        };
    }

    std::vector<String> l = bruceConfig.disabledMenus;
    for (int i = 0; i < _totalItems; i++) {
        String itemName = _menuItems[i]->getName();
        if (find(l.begin(), l.end(), itemName) == l.end()) { // If menu item is not disabled
            if (_menuItems[i] == &rfMenu && !mqModules.cc1101) {
                options.push_back(
                    {"RF [N/A]",
                     [this]() {
                         if (probeCC1101(true)) {
                             rfMenu.optionsMenu();
                         } else {
                             displayWarning("CC1101 not detected (SPI)", true);
                         }
                     },
                     false,
                     renderItem,
                     _menuItems[i]}
                );
            } else if (_menuItems[i] == &rfidMenu && !mqModules.pn532) {
                options.push_back(
                    {"RFID [N/A]",
                     [this]() {
                         if (probePN532(true)) {
                             rfidMenu.optionsMenu();
                         } else {
                             displayWarning("PN532 not detected (I2C)", true);
                         }
                     },
                     false,
                     renderItem,
                     _menuItems[i]}
                );
            } else {
                options.push_back(
                    {// selected lambda
                     itemName,
                     [this, i]() { _menuItems[i]->optionsMenu(); },
                     false, // selected = false
                     renderItem,
                     _menuItems[i]
                    }
                );
            }
        }
    }

    options.push_back(
        {"Modules",
         []() { showModuleStatus(); },
         false,
         renderItem,
         &mqModulesMenuItem}
    );

    // The grid/list need a fresh full repaint every time we come back to the main menu,
    // since loopOptions clears the screen before the first render.
    if (gridStyle) {
        buildGridLayout(options.size());
        _gridScroll = 0;
        _gridLastIndex = -1;
        _gridRedrawAll = true;
        mainMenuGridColumns = _grid.cols;
    } else if (listStyle) {
        _listScroll = 0;
        _listLastIndex = -1;
        _listRedrawAll = true;
        mainMenuGridColumns = 0;
    } else {
        mainMenuGridColumns = 0;
    }

    int sel = loopOptions(options, MENU_TYPE_MAIN, "Main Menu", _currentIndex);
    if (sel >= 0) _currentIndex = sel;
};

void MainMenu::drawList(int index) {
    int itemCount = options.size();
    if (itemCount < 1) return;
    if (index < 0 || index >= itemCount) index = 0;

    int areaX = BORDER_OFFSET_FROM_SCREEN_EDGE + 4;
    int areaY = STATUS_BAR_HEIGHT - 1;
    int scrollW = 6;
    int rowW = tftWidth - 2 * areaX - scrollW;
    int areaH = tftHeight - areaY - BORDER_OFFSET_FROM_SCREEN_EDGE - 1;
    int visibleRows = max(1, min(itemCount, 6));
    int rowH = max(20, areaH / visibleRows);

    if (_listRotation != bruceConfigPins.rotation) {
        _listRotation = bruceConfigPins.rotation;
        _listRedrawAll = true;
    }

    int scroll = _listScroll;
    if (index < scroll) scroll = index;
    else if (index >= scroll + visibleRows) scroll = index - visibleRows + 1;
    int maxScroll = max(0, itemCount - visibleRows);
    if (scroll > maxScroll) scroll = maxScroll;

    bool redrawAll = _listRedrawAll || scroll != _listScroll;
    _listScroll = scroll;

    if (redrawAll) {
        for (auto &opt : options) {
            opt.w = 0;
            opt.h = 0;
        }
        drawMainBorder(false);
        tft.fillRect(areaX, areaY, rowW, visibleRows * rowH, bruceConfig.bgColor);
        int first = _listScroll;
        int last = min(itemCount, first + visibleRows);
        for (int i = first; i < last; i++) {
            drawListRow(i, i - first, areaX, areaY, rowW, rowH, i == index);
        }
        drawListScrollBar(areaY, visibleRows * rowH, visibleRows, itemCount);
#if defined(HAS_TOUCH)
        TouchFooter();
#endif
    } else {
        if (_listLastIndex >= _listScroll && _listLastIndex < _listScroll + visibleRows && _listLastIndex != index) {
            drawListRow(_listLastIndex, _listLastIndex - _listScroll, areaX, areaY, rowW, rowH, false);
        }
        drawListRow(index, index - _listScroll, areaX, areaY, rowW, rowH, true);
        drawListScrollBar(areaY, visibleRows * rowH, visibleRows, itemCount);
    }

    _listLastIndex = index;
    _listRedrawAll = false;
}

void MainMenu::drawListRow(int index, int row, int areaX, int areaY, int rowW, int rowH, bool selected) {
    int y = areaY + row * rowH;
    options[index].x = areaX;
    options[index].y = y;
    options[index].w = rowW;
    options[index].h = rowH;

    uint16_t rowBg = selected ? bruceConfig.priColor : bruceConfig.bgColor;
    uint16_t txtFg = selected ? TFT_BLACK : bruceConfig.priColor;
    int badgeSize = min(rowH - 6, 22);
    int iconX = areaX + 4 + badgeSize / 2;
    int iconY = y + rowH / 2;

    tft.fillRect(areaX, y, rowW, rowH, bruceConfig.bgColor);
    if (selected) {
        tft.fillRoundRect(areaX, y + 1, rowW, rowH - 2, 5, rowBg);
        tft.fillRoundRect(areaX + 3, y + (rowH - badgeSize) / 2, badgeSize, badgeSize, 4, bruceConfig.bgColor);
    }

    MenuItemInterface *item = static_cast<MenuItemInterface *>(options[index].hoverPointer);
    if (item) {
        item->drawIconInBox(iconX, iconY, badgeSize - 2, bruceConfig.priColor, bruceConfig.bgColor);
    }

    int textX = areaX + badgeSize + 10;
    int maxChars = max(1, (rowW - (textX - areaX) - 14) / (LW * FM));
    tft.setTextSize(FM);
    tft.setTextColor(txtFg, rowBg);
    tft.drawString(options[index].label.substring(0, maxChars), textX, y + (rowH - LH * FM) / 2);
    if (selected) {
        tft.drawString(">", areaX + rowW - 12, y + (rowH - LH * FM) / 2);
    }
}

void MainMenu::drawListScrollBar(int areaY, int trackH, int visibleRows, int itemCount) {
    int barX = tftWidth - BORDER_OFFSET_FROM_SCREEN_EDGE - 4;
    tft.fillRect(barX, areaY, 3, trackH, bruceConfig.bgColor);
    if (itemCount <= visibleRows) return;
    int thumbH = max(8, trackH * visibleRows / itemCount);
    int thumbY = areaY + (trackH - thumbH) * _listScroll / max(1, itemCount - visibleRows);
    tft.fillRect(barX, thumbY, 3, thumbH, bruceConfig.priColor);
}

/*********************************************************************
**  Function: gridIndexOf
**  Position of a menu item inside the currently built options list.
**  The hover callback is a plain function pointer and cannot capture the
**  index, so it is resolved back from the item pointer it receives.
**********************************************************************/
int MainMenu::gridIndexOf(MenuItemInterface *item) {
    for (size_t i = 0; i < options.size(); i++) {
        if (options[i].hoverPointer == item) return (int)i;
    }
    return 0;
}

/*********************************************************************
**  Function: buildGridLayout
**  Picks the column count that makes cells (and therefore icons) as large as possible for the
**  available area, instead of just cramming in as many columns as fit at the minimum size. Only
**  falls back to the minimum size, with paging, once even one column can't make cells that big.
**********************************************************************/
void MainMenu::buildGridLayout(int itemCount) {
    // Floor so a cell never gets too small to tap/read — this is what triggers paging, not a
    // preferred size. Scaled by FP so it still means something on high-res boards.
    const int MIN_CELL_W = 54 * FP;
    const int MIN_CELL_H = 40 * FP;
    // Right-edge column reserved for the scrollbar and the page-up/page-down tap zone (see
    // MainMenu::handleGridPageTap()) — wide enough to actually tap, not just the 3px bar itself.
    const int PAGE_TAP_W = 4 * BORDER_OFFSET_FROM_SCREEN_EDGE;

    if (itemCount < 1) itemCount = 1;

    int areaX = BORDER_OFFSET_FROM_SCREEN_EDGE + 2;
    int areaY = STATUS_BAR_HEIGHT - 2; // first line free under the status bar separator
    int areaW = tftWidth - 2 * areaX - PAGE_TAP_W;
    int areaH = tftHeight - areaY - areaX;

    // Try every column count down to the MIN_CELL_W floor and keep the one whose resulting cells
    // are largest in their smaller dimension (min(cellW, cellH)) — that's what a lopsided grid
    // wastes: a handful of very wide, short rows or very tall, narrow columns both score low here,
    // while a balanced, roughly-square cell that actually fills the screen scores high.
    int maxCols = max(1, min(itemCount, areaW / MIN_CELL_W));
    int bestCols = 1;
    int bestScore = -1;
    for (int cols = 1; cols <= maxCols; cols++) {
        int rows = (itemCount + cols - 1) / cols;
        int score = min(areaW / cols, areaH / rows);
        if (score > bestScore) {
            bestScore = score;
            bestCols = cols;
        }
    }

    _grid.cols = bestCols;
    _grid.cellW = areaW / _grid.cols;
    _grid.rows = (itemCount + _grid.cols - 1) / _grid.cols;

    // Rows still get capped to what fits at MIN_CELL_H — that's the paging trigger when there
    // are simply too many items for one screen even at the smallest usable size.
    _grid.visibleRows = max(1, areaH / MIN_CELL_H);
    if (_grid.visibleRows > _grid.rows) _grid.visibleRows = _grid.rows;
    _grid.cellH = areaH / _grid.visibleRows; // fills whatever height is actually available

    _grid.labelSize = _grid.cellW >= 8 * LW * FM ? FM : FP;
    // Everything the cell has left once the label and a thin margin are taken out
    _grid.iconBox = _grid.cellH - LH * _grid.labelSize - 6;
    if (_grid.iconBox > _grid.cellW - 4) _grid.iconBox = _grid.cellW - 4; // narrow cells cap it
    if (_grid.iconBox < 12) _grid.iconBox = 12;

    // Center whatever is left over so the grid doesn't hug the border
    _grid.x = areaX + (areaW - _grid.cols * _grid.cellW) / 2;
    _grid.y = areaY + (areaH - _grid.visibleRows * _grid.cellH) / 2;

    _gridItemCount = itemCount;
    _gridRotation = bruceConfigPins.rotation;
}

/*********************************************************************
**  Function: drawGrid
**  Draws the module grid highlighting `index`. Only the two cells that
**  changed are repainted unless the whole view has to come back.
**********************************************************************/
void MainMenu::drawGrid(int index) {
    int itemCount = options.size();
    if (itemCount < 1) return;
    if (index < 0 || index >= itemCount) index = 0;

    if (itemCount != _gridItemCount || _gridRotation != bruceConfigPins.rotation) {
        buildGridLayout(itemCount);
        _gridScroll = 0;
        _gridLastIndex = -1;
        _gridRedrawAll = true;
    }

    // Keep the selected cell inside the visible window
    int scroll = _gridScroll;
    int row = index / _grid.cols;
    if (row < scroll) scroll = row;
    else if (row >= scroll + _grid.visibleRows) scroll = row - _grid.visibleRows + 1;

    bool redrawAll = _gridRedrawAll || scroll != _gridScroll;
    _gridScroll = scroll;

    if (redrawAll) {
        // Cells scrolled out of the visible window won't get a drawGridCell() call below to refresh
        // their tap rect, so clear everyone's first (mirrors drawOptions()'s same stale-rect concern).
        for (auto &opt : options) {
            opt.w = 0;
            opt.h = 0;
        }
        drawMainBorder(false);
        tft.fillRect(
            _grid.x, _grid.y, _grid.cols * _grid.cellW, _grid.visibleRows * _grid.cellH, bruceConfig.bgColor
        );
        int first = _gridScroll * _grid.cols;
        int last = min(itemCount, first + _grid.visibleRows * _grid.cols);
        for (int i = first; i < last; i++) drawGridCell(i, i == index);
        drawGridScrollBar();
#if defined(HAS_TOUCH)
        TouchFooter();
#endif
    } else {
        if (_gridLastIndex >= 0 && _gridLastIndex != index && _gridLastIndex < itemCount)
            drawGridCell(_gridLastIndex, false);
        drawGridCell(index, true);
    }

    _gridLastIndex = index;
    _gridRedrawAll = false;
}

/*********************************************************************
**  Function: drawGridCell
**  Paints a single module cell, inverted when it is the selected one
**********************************************************************/
void MainMenu::drawGridCell(int index, bool selected) {
    int row = index / _grid.cols - _gridScroll;
    int col = index % _grid.cols;
    if (row < 0 || row >= _grid.visibleRows) return;

    int x = _grid.x + col * _grid.cellW;
    int y = _grid.y + row * _grid.cellH;

    options[index].x = x;
    options[index].y = y;
    options[index].w = _grid.cellW;
    options[index].h = _grid.cellH;

    uint16_t bgColor = selected ? bruceConfig.priColor : bruceConfig.bgColor;
    uint16_t fgColor = selected ? bruceConfig.bgColor : bruceConfig.priColor;

    tft.fillRect(x, y, _grid.cellW, _grid.cellH, bruceConfig.bgColor);
    if (selected) tft.fillRoundRect(x + 1, y + 1, _grid.cellW - 2, _grid.cellH - 2, 3, bgColor);

    MenuItemInterface *item = static_cast<MenuItemInterface *>(options[index].hoverPointer);
    if (item)
        item->drawIconInBox(x + _grid.cellW / 2, y + 3 + _grid.iconBox / 2, _grid.iconBox, fgColor, bgColor);

    int maxChars = (_grid.cellW - 4) / (LW * _grid.labelSize);
    tft.setTextSize(_grid.labelSize);
    tft.setTextColor(fgColor, bgColor);
    tft.drawCentreString(
        options[index].label.substring(0, maxChars),
        x + _grid.cellW / 2,
        y + _grid.cellH - LH * _grid.labelSize - 3,
        1
    );
}

/*********************************************************************
**  Function: drawGridScrollBar
**  Thin indicator on the right telling there are more rows off screen
**********************************************************************/
void MainMenu::drawGridScrollBar() {
    int barX = tftWidth - BORDER_OFFSET_FROM_SCREEN_EDGE - 4;
    int trackY = _grid.y;
    int trackH = _grid.visibleRows * _grid.cellH;

    tft.fillRect(barX, trackY, 3, trackH, bruceConfig.bgColor);
    if (_grid.rows <= _grid.visibleRows) return;

    int thumbH = max(6, trackH * _grid.visibleRows / _grid.rows);
    int thumbY = trackY + (trackH - thumbH) * _gridScroll / (_grid.rows - _grid.visibleRows);
    tft.fillRect(barX, thumbY, 3, thumbH, bruceConfig.priColor);
}

/*********************************************************************
**  Function: handleGridPageTap
**  Right-edge strip (see PAGE_TAP_W in buildGridLayout): tapping the top half jumps a full page
**  up, the bottom half a full page down. Needed because a tap can only select a cell that's
**  already on screen, unlike Prev/Next which auto-scroll into view one cell at a time.
**********************************************************************/
bool MainMenu::handleGridPageTap(int x, int y, int currentIndex, int &newIndex) {
    if (_grid.rows <= _grid.visibleRows) return false; // everything already fits on one page

    int zoneX = _grid.x + _grid.cols * _grid.cellW;
    int zoneY = _grid.y;
    int zoneW = tftWidth - BORDER_OFFSET_FROM_SCREEN_EDGE - zoneX;
    int zoneH = _grid.visibleRows * _grid.cellH;
    if (x < zoneX || x >= zoneX + zoneW || y < zoneY || y >= zoneY + zoneH) return false;

    int maxScroll = _grid.rows - _grid.visibleRows;
    int targetScroll = (y < zoneY + zoneH / 2) ? max(0, _gridScroll - _grid.visibleRows)
                                               : min(maxScroll, _gridScroll + _grid.visibleRows);

    // Keep the same column, jump to the first row of the new page.
    int col = currentIndex % _grid.cols;
    int itemCount = static_cast<int>(options.size());
    newIndex = min(targetScroll * _grid.cols + col, itemCount - 1);
    return true;
}

/*********************************************************************
**  Function: hideAppsMenu
**  Menu to Hide or show menus
**********************************************************************/

void MainMenu::hideAppsMenu() {
    auto items = this->getItems();
    int index = 0;
RESTART: // using gotos to avoid stackoverflow after many choices
    options.clear();
    for (auto item : items) {
        String label = item->getName();
        std::vector<String> l = bruceConfig.disabledMenus;
        bool enabled = find(l.begin(), l.end(), label) == l.end();
        options.push_back(
            {label,
             [this, label, enabled]() {
                 if (enabled) bruceConfig.addDisabledMenu(label);
                 else bruceConfig.removeDisabledMenu(label);
             },
             enabled}
        );
    }
    options.push_back({"Show All", [=]() { bruceConfig.disabledMenus.clear(); }, true});
    addOptionToMainMenu();
    index = loopOptions(options, index);
    bruceConfig.saveFile();
    if (!returnToMenu) goto RESTART;
}
