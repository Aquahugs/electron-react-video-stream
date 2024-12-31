const { contextBridge, ipcRenderer } = require('electron');

// Expose a protected 'electron' API to the renderer process
contextBridge.exposeInMainWorld('electron', {
  on: (channel, func) => {
    console.log(`Registering listener for channel: ${channel}`);
    if (channel === 'frame-data') {
      // Explicitly wrap the frame-data handler
      ipcRenderer.on(channel, (event, ...args) => {
        console.log('Received frame in preload, size:', args[0]?.length);
        func(...args);
      });
    }
  }
});