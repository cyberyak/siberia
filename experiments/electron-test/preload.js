const { sharedTexture, ipcRenderer } = require('electron');
window.siberia = { sharedTexture, ipc: ipcRenderer };
