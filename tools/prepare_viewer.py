"""Add an opt-in minimum keyboard press duration to the pinned private viewer."""
from pathlib import Path
import hashlib
import re

ROOT=Path(__file__).resolve().parents[1]
MARKER='// Rocket bridge: opt-in keyboard timing for polled game input.'

def prepare(client=ROOT/'.deps/noVNC'):
    path=client/'core/rfb.js'
    text=path.read_text()
    if MARKER in text:
        prepare_mouse(client)
        prepare_buttons(client)
        return
    signature='    sendKey(keysym, code, down) {'
    anchor='        const scancode = XtScancode[code];'
    if text.count(signature)!=1 or text.count(anchor)!=1:
        raise RuntimeError('Unexpected noVNC keyboard implementation')
    text=text.replace(signature,'    sendKey(keysym, code, down, delayed = false) {')
    patch='''        // Rocket bridge: opt-in keyboard timing for polled game input.
        const holdMs = Math.min(100, Math.max(0,
            Number(new URLSearchParams(window.location.search).get('key_hold_ms')) || 0));
        if (holdMs && !delayed) {
            this._bridgeKeys ??= new Map();
            const id = code || keysym;
            const previous = this._bridgeKeys.get(id);
            if (down) {
                if (previous) clearTimeout(previous.timer);
                this._bridgeKeys.set(id, { started: performance.now() });
            } else if (previous) {
                const remaining = holdMs - (performance.now() - previous.started);
                if (remaining > 0) {
                    previous.timer = setTimeout(() => {
                        this._bridgeKeys.delete(id);
                        this.sendKey(keysym, code, false, true);
                    }, remaining);
                    return;
                }
                this._bridgeKeys.delete(id);
            }
        }

'''
    path.write_text(text.replace(anchor,patch+anchor))
    prepare_mouse(client)
    prepare_buttons(client)

def prepare_mouse(client):
    path=client/'core/rfb.js';text=path.read_text()
    marker='// Rocket bridge: minimum pointer hold for polled menus.'
    if marker in text:return
    anchor='    _handleMouseButton(x, y, bmask) {\n'
    patch='''        // Rocket bridge: minimum pointer hold for polled menus.
        const hold = Math.min(500, Math.max(0,
            Number(new URLSearchParams(location.search).get("mouse_hold_ms")) || 0));
        if (hold && !delayed) {
            if (bmask) this._bridgeMouseStarted = performance.now();
            else if (this._mouseButtonMask) {
                const remaining = hold - (performance.now() - this._bridgeMouseStarted);
                if (remaining > 0) {
                    setTimeout(() => this._handleMouseButton(x, y, bmask, true), remaining);
                    return;
                }
            }
        }
'''
    if text.count(anchor)!=1:raise RuntimeError('Unexpected noVNC pointer implementation')
    text=text.replace(anchor,'    _handleMouseButton(x, y, bmask, delayed = false) {\n'+patch)
    path.write_text(text)

def prepare_buttons(client):
    prepare_drive(client)
    path=client/'app/ui.js';text=path.read_text()
    text=text.replace("['F8', KeyTable.XK_F8, 'F8'],", "['F8', KeyTable.XK_F8, 'F8'],\n            ['Activate', KeyTable.XK_e, 'KeyE'],\n            ['Next choice', KeyTable.XK_d, 'KeyD'],") if "['Activate'" not in text else text
    text=text.replace('code, false), 80);','code, false), 300);').replace('code, false), 100);','code, false), 300);')
    path.write_text(text)
    marker='// Rocket bridge: explicit remote console keys.'
    if marker in text:
        prepare_commands(client)
        return
    anchor='    addExtraKeysHandlers() {\n'
    if text.count(anchor)!=1:raise RuntimeError('Unexpected noVNC controls implementation')
    patch='''        // Rocket bridge: explicit remote console keys.
        for (const [label, keysym, code] of [
            ['Enter', KeyTable.XK_Return, 'Enter'],
            ['Backspace', KeyTable.XK_BackSpace, 'Backspace'],
            ['Console', KeyTable.XK_grave, 'Backquote'],
            ['F8', KeyTable.XK_F8, 'F8'],
            ['Activate', KeyTable.XK_e, 'KeyE'],
            ['Next choice', KeyTable.XK_d, 'KeyD'],
        ]) {
            const button = document.createElement('input');
            button.type = 'button'; button.value = label;
            button.title = 'Send ' + label; button.className = 'noVNC_button';
            button.addEventListener('click', () => {
                if (!UI.rfb) return;
                UI.rfb.sendKey(keysym, code, true);
                setTimeout(() => UI.rfb?.sendKey(keysym, code, false), 300);
            });
            document.getElementById('noVNC_modifiers').append(button);
        }
'''
    path.write_text(text.replace(anchor,anchor+patch))
    prepare_commands(client)

def prepare_commands(client):
    path=client/'app/ui.js';text=path.read_text()
    marker='// Rocket bridge: paced text for game consoles.'
    if marker not in text:
        anchor='    addExtraKeysHandlers() {\n'
        patch='''        // Rocket bridge: paced text for game consoles.
        const command = document.createElement('input');
        command.type = 'text'; command.setAttribute('aria-label', 'Console command');
        command.placeholder = 'Open console first'; command.style.width = '170px';
        const run = document.createElement('input');
        run.type = 'button'; run.value = 'Run command'; run.className = 'noVNC_button';
        run.addEventListener('click', async () => {
            const value = command.value;
            if (!UI.rfb || !value || !/^[a-zA-Z0-9 _.;=-]+$/.test(value)) return;
            run.disabled = true;
            const key = async (keysym, code) => {
                UI.rfb?.sendKey(keysym, code, true);
                await new Promise(resolve => setTimeout(resolve, 100));
                UI.rfb?.sendKey(keysym, code, false);
                await new Promise(resolve => setTimeout(resolve, 60));
            };
            try {
                // Replace only the console's current input line.
                for (let i = 0; i < 32; ++i) await key(KeyTable.XK_BackSpace, 'Backspace');
                for (const c of value) {
                    const code = /[a-zA-Z]/.test(c) ? 'Key' + c.toUpperCase() :
                        /[0-9]/.test(c) ? 'Digit' + c : c === ' ' ? 'Space' : undefined;
                    await key(c.codePointAt(0), code);
                }
                await key(KeyTable.XK_Return, 'Enter');
            } finally { run.disabled = false; }
        });
        document.getElementById('noVNC_modifiers').append(command, run);
'''
        if text.count(anchor)!=1:raise RuntimeError('Unexpected noVNC controls implementation')
        path.write_text(text.replace(anchor,anchor+patch))
    prepare_export(client)

def prepare_export(client):
    path=client/'app/ui.js';text=path.read_text()
    text=text.replace("link.download = 'rocket-skyrim-view.png';", "link.download = 'rocket-skyrim-view-' + Date.now() + '.png';")
    path.write_text(text)
    marker='// Rocket bridge: export the private game view.'
    if marker not in text:
        anchor='    addExtraKeysHandlers() {\n'
        patch='''        // Rocket bridge: export the private game view.
        const saveView = document.createElement('input');
        saveView.type = 'button'; saveView.value = 'Save view'; saveView.className = 'noVNC_button';
        saveView.addEventListener('click', () => {
            const canvas = document.querySelector('#noVNC_container canvas');
            if (!canvas) return;
            const link = document.createElement('a');
            link.href = canvas.toDataURL('image/png'); link.download = 'rocket-skyrim-view-' + Date.now() + '.png';
            link.click();
        });
        document.getElementById('noVNC_modifiers').append(saveView);
'''
        if text.count(anchor)!=1:raise RuntimeError('Unexpected noVNC controls implementation')
        path.write_text(text.replace(anchor,anchor+patch))
    prepare_navigation(client)

def prepare_navigation(client):
    path=client/'app/ui.js';text=path.read_text()
    marker='// Rocket bridge: paced menu navigation keys.'
    if marker not in text:
        anchor='    addExtraKeysHandlers() {\n'
        patch='''        // Rocket bridge: paced menu navigation keys.
        for (const [label, keysym, code] of [
            ['Left', KeyTable.XK_Left, 'ArrowLeft'], ['Right', KeyTable.XK_Right, 'ArrowRight'],
            ['Up', KeyTable.XK_Up, 'ArrowUp'], ['Down', KeyTable.XK_Down, 'ArrowDown'],
            ['F6', KeyTable.XK_F6, 'F6'], ['F10', KeyTable.XK_F10, 'F10'],
        ]) {
            const button = document.createElement('input');
            button.type = 'button'; button.value = label; button.className = 'noVNC_button';
            button.addEventListener('click', () => {
                if (!UI.rfb) return;
                UI.rfb.sendKey(keysym, code, true);
                setTimeout(() => UI.rfb?.sendKey(keysym, code, false), 300);
            });
            document.getElementById('noVNC_modifiers').append(button);
        }
'''
        if text.count(anchor)!=1:raise RuntimeError('Unexpected noVNC controls implementation')
        path.write_text(text.replace(anchor,anchor+patch))
    refresh_imports(client)

def refresh_imports(client):
    path=client/'app/ui.js';text=path.read_text()
    revision=hashlib.sha256((client/'core/rfb.js').read_bytes()).hexdigest()[:12]
    updated=re.sub(r'import RFB from "\.\./core/rfb\.js(?:\?bridge=[a-f0-9]+)?";',f'import RFB from "../core/rfb.js?bridge={revision}";',text)
    if updated!=text:path.write_text(updated)
    revision=hashlib.sha256(path.read_bytes()).hexdigest()[:12]
    page=client/'vnc.html';text=page.read_text()
    updated=re.sub(r'import UI from "\./app/ui\.js(?:\?bridge=[a-f0-9]+)?";',f'import UI from "./app/ui.js?bridge={revision}";',text)
    if updated!=text:page.write_text(updated)



def prepare_drive(client):
    path=client/'app/ui.js';text=path.read_text()
    marker='// Rocket bridge: bounded RL drive controls.'
    if marker in text:return
    anchor='    addExtraKeysHandlers() {\n'
    patch='''        // Rocket bridge: bounded RL drive controls.
        for (const [label, keysym, code, duration] of [
            ['RL forward (1s)', KeyTable.XK_w, 'KeyW', 1000],
            ['RL reverse (1s)', KeyTable.XK_s, 'KeyS', 1000],
            ['RL left (1s)', KeyTable.XK_a, 'KeyA', 1000],
            ['RL right (1s)', KeyTable.XK_d, 'KeyD', 1000],
            ['RL jump', KeyTable.XK_space, 'Space', 300],
        ]) {
            const b=document.createElement('input');b.type='button';b.value=label;b.className='noVNC_button';
            b.addEventListener('click',()=> {if(!UI.rfb || b.disabled)return;b.disabled=true;UI.rfb.sendKey(keysym,code,true);setTimeout(()=> {UI.rfb?.sendKey(keysym,code,false);b.disabled=false;},duration);});
            document.getElementById('noVNC_modifiers').append(b);
        }
'''
    if text.count(anchor)!=1:raise RuntimeError('Unexpected noVNC controls implementation')
    path.write_text(text.replace(anchor,anchor+patch))


if __name__ == "__main__":
    prepare()
