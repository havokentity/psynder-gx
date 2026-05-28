// SPDX-License-Identifier: MIT
// PsyEditorGX — dedicated Chromium shell for the Psynder-GX editor workbench.

const { app, BrowserWindow, Menu, shell } = require('electron');
const os = require('node:os');
const path = require('node:path');
const { pathToFileURL } = require('node:url');

function arg_value(name) {
  const prefix = `${name}=`;
  const found = process.argv.find((arg) => arg.startsWith(prefix));
  if (found) return found.slice(prefix.length);
  const index = process.argv.indexOf(name);
  return index >= 0 && index + 1 < process.argv.length
    ? process.argv[index + 1]
    : '';
}

function editor_url() {
  const explicit_url = arg_value('--url') || process.env.PSYNDER_GX_EDITOR_URL;
  if (explicit_url) return explicit_url;

  const dist = arg_value('--dist') ||
    process.env.PSYNDER_GX_EDITOR_DIST ||
    path.join(__dirname, '..', 'dist');
  return pathToFileURL(path.join(dist, 'index.html')).toString();
}

function user_data_dir() {
  const override = process.env.PSYNDER_GX_EDITOR_USER_DATA;
  if (override) return override;
  if (process.platform === 'darwin') {
    return path.join(os.homedir(), 'Library', 'Application Support', 'Psynder-GX', 'PsyEditorGX');
  }
  if (process.platform === 'win32') {
    return path.join(process.env.APPDATA || os.homedir(), 'Psynder-GX', 'PsyEditorGX');
  }
  return path.join(process.env.XDG_CONFIG_HOME || path.join(os.homedir(), '.config'), 'Psynder-GX', 'PsyEditorGX');
}

const target_url = editor_url();
app.setName('PsyEditorGX');
app.setPath('userData', user_data_dir());
app.commandLine.appendSwitch('disable-features', 'AutofillServerCommunication');

const got_lock = app.requestSingleInstanceLock({ url: target_url });
if (!got_lock) {
  app.quit();
}

let main_window = null;

function create_window() {
  Menu.setApplicationMenu(null);
  main_window = new BrowserWindow({
    width: 1180,
    height: 1040,
    minWidth: 900,
    minHeight: 620,
    title: 'PsyEditorGX',
    backgroundColor: '#071015',
    autoHideMenuBar: true,
    show: false,
    webPreferences: {
      nodeIntegration: false,
      contextIsolation: true,
      sandbox: true,
      spellcheck: false,
      devTools: true,
    },
  });

  main_window.once('ready-to-show', () => {
    main_window.show();
  });

  main_window.webContents.setWindowOpenHandler(({ url }) => {
    if (url.startsWith('http://127.0.0.1:') || url.startsWith('https://127.0.0.1:')) {
      return { action: 'allow' };
    }
    shell.openExternal(url).catch(() => {});
    return { action: 'deny' };
  });

  main_window.webContents.on('before-input-event', (event, input) => {
    const mod = process.platform === 'darwin' ? input.meta : input.control;
    if (input.type !== 'keyDown') return;
    if (input.key === 'F12' || (mod && input.shift && input.key.toLowerCase() === 'i')) {
      main_window.webContents.toggleDevTools();
      event.preventDefault();
    } else if (mod && input.key.toLowerCase() === 'r') {
      main_window.webContents.reloadIgnoringCache();
      event.preventDefault();
    }
  });

  main_window.loadURL(target_url).catch(() => {
    main_window.loadURL(`data:text/html;charset=utf-8,${encodeURIComponent(failure_html(target_url))}`)
      .catch(() => {});
  });
}

function failure_html(url) {
  return `<!doctype html>
<meta charset="utf-8">
<title>PsyEditorGX</title>
<style>
  html,body{margin:0;height:100%;background:#071015;color:#d6f4ff;font:14px ui-monospace,SFMono-Regular,Menlo,Consolas,monospace}
  main{height:100%;display:grid;place-content:center;gap:16px;text-align:center}
  h1{font-size:20px;letter-spacing:.08em;text-transform:uppercase}
  code{color:#8ee7ff}
  button{background:#8ee7ff;border:0;color:#061116;padding:10px 14px;font:inherit}
</style>
<main>
  <h1>PsyEditorGX could not reach the workbench</h1>
  <p>Target <code>${url}</code></p>
  <button onclick="location.href='${url}'">Retry</button>
</main>`;
}

app.whenReady().then(create_window);

app.on('second-instance', (_event, argv, _cwd, data) => {
  const next_url = data && typeof data.url === 'string'
    ? data.url
    : target_url;
  if (main_window) {
    if (main_window.isMinimized()) main_window.restore();
    main_window.focus();
    main_window.loadURL(next_url).catch(() => {});
  }
});

app.on('window-all-closed', () => {
  app.quit();
});
