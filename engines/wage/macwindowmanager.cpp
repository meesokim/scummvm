/* ScummVM - Graphic Adventure Engine
 *
 * ScummVM is the legal property of its developers, whose names
 * are too numerous to list here. Please refer to the COPYRIGHT
 * file distributed with this source distribution.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * MIT License:
 *
 * Copyright (c) 2009 Alexei Svitkine, Eugene Sandulenko
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include "common/array.h"
#include "common/events.h"
#include "common/list.h"
#include "common/system.h"

#include "graphics/managed_surface.h"

#include "wage/wage.h"
#include "wage/design.h"
#include "wage/gui.h"
#include "wage/macwindow.h"
#include "wage/macwindowmanager.h"
#include "wage/menu.h"

namespace Wage {

enum {
	kPatternCheckers = 1
};

static byte fillPatterns[][8] = { { 0xaa, 0x55, 0xaa, 0x55, 0xaa, 0x55, 0xaa, 0x55 } // kPatternCheckers
};


MacWindowManager::MacWindowManager() {
    _screen = 0;
    _lastId = 0;
    _activeWindow = -1;

	_menu = 0;

	_fullRefresh = true;

	for (int i = 0; i < ARRAYSIZE(fillPatterns); i++)
		_patterns.push_back(fillPatterns[i]);
}

MacWindowManager::~MacWindowManager() {
    for (int i = 0; i < _lastId; i++)
        delete _windows[i];
}

MacWindow *MacWindowManager::addWindow(bool scrollable, bool resizable) {
    MacWindow *w = new MacWindow(_lastId, scrollable, resizable);

    _windows.push_back(w);
    _windowStack.push_back(w);

    setActive(_lastId);

    _lastId++;

    return w;
}

Menu *MacWindowManager::addMenu(Gui *g) {
	_menu = new Menu(_lastId, g);

	_windows.push_back(_menu);

	_lastId++;

	return _menu;
}

void MacWindowManager::setActive(int id) {
    if (_activeWindow == id)
        return;

    if (_activeWindow != -1)
        _windows[_activeWindow]->setActive(false);

    _activeWindow = id;

    _windows[id]->setActive(true);

    _windowStack.remove(_windows[id]);
    _windowStack.push_back(_windows[id]);

    _fullRefresh = true;
}

void MacWindowManager::draw() {
    assert(_screen);

	if (_fullRefresh)
		drawDesktop();

    for (Common::List<BaseMacWindow *>::const_iterator it = _windowStack.begin(); it != _windowStack.end(); it++) {
        BaseMacWindow *w = *it;
        if (w->draw(_screen, _fullRefresh)) {
            w->setDirty(false);

			Common::Rect clip(w->getDimensions().left - 2, w->getDimensions().top - 2, w->getDimensions().right - 2, w->getDimensions().bottom - 2);
			clip.clip(_screen->getBounds());

            g_system->copyRectToScreen(_screen->getBasePtr(clip.left, clip.top), _screen->pitch, clip.left, clip.top, clip.width(), clip.height());
        }
    }

	// Menu is drawn on top of everything and always
	if (_menu)
		_menu->draw(_screen, _fullRefresh);

    _fullRefresh = false;
}

void MacWindowManager::drawDesktop() {
	Common::Rect r(_screen->getBounds());

	Design::drawFilledRoundRect(_screen, r, kDesktopArc, kColorBlack, _patterns, kPatternCheckers);
	g_system->copyRectToScreen(_screen->getPixels(), _screen->pitch, 0, 0, _screen->w, _screen->h);
}

bool MacWindowManager::processEvent(Common::Event &event) {
	if (_menu && _menu->processEvent(event))
		return true;

    if (event.type != Common::EVENT_MOUSEMOVE && event.type != Common::EVENT_LBUTTONDOWN &&
            event.type != Common::EVENT_LBUTTONUP)
        return false;

    for (Common::List<BaseMacWindow *>::const_iterator it = _windowStack.end(); it != _windowStack.begin();) {
        it--;
        BaseMacWindow *w = *it;

        if (w->hasAllFocus() || w->getDimensions().contains(event.mouse.x, event.mouse.y)) {
            if (event.type == Common::EVENT_LBUTTONDOWN || event.type == Common::EVENT_LBUTTONUP)
                setActive(w->getId());

            return w->processEvent(event);
        }
    }

    return false;
}

} // End of namespace Wage