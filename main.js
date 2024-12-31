const { app, BrowserWindow, ipcMain } = require('electron');
const path = require('path');
const WebSocket = require('ws');

let mainWindow;

const FRAME_SIZE = 1244160; // Exact NV12 frame size for 710x1080

function createWindow() {
  mainWindow = new BrowserWindow({
    width: 1280,
    height: 720,
    webPreferences: {
      nodeIntegration: false,
      contextIsolation: true,
      preload: path.join(__dirname, 'preload.js')
    },
  });

  mainWindow.loadURL('http://localhost:8080');
  mainWindow.webContents.openDevTools();
}

function createWebSocketServer() {
  const width = 710;
  const height = 1080;
  const FRAME_SIZE = width * height * 4; // 4 bytes per pixel for RGBA
  
  console.log(`Setting up WebSocket server for RGBA frames (${width}x${height}, ${FRAME_SIZE} bytes per frame)`);
  
  const wss = new WebSocket.Server({ 
    port: 8081,
    perMessageDeflate: false,
    maxPayload: FRAME_SIZE
  });
  
  wss.on('connection', (ws) => {
    console.log('WebSocket client connected');
    
    ws.binaryType = 'nodebuffer';
    let frameBuffer = Buffer.alloc(0);

    if (ws._socket) {
      ws._socket.setNoDelay(true);
      ws._socket.setKeepAlive(true, 30000);
    }

    ws.on('message', (message) => {
      try {
        frameBuffer = Buffer.concat([frameBuffer, message]);
        
        // If we have a complete RGBA frame
        if (frameBuffer.length >= FRAME_SIZE) {
          console.log('Sending complete RGBA frame');
          if (mainWindow && !mainWindow.isDestroyed()) {
            mainWindow.webContents.send('frame-data', frameBuffer.slice(0, FRAME_SIZE));
          }
          frameBuffer = frameBuffer.slice(FRAME_SIZE);
        }
      } catch (err) {
        console.error('Error processing message:', err);
        frameBuffer = Buffer.alloc(0);
      }
    });

    ws.on('error', (error) => {
      console.error('WebSocket error:', error);
      frameBuffer = Buffer.alloc(0);
    });

    ws.on('close', () => {
      console.log('Client disconnected');
      frameBuffer = Buffer.alloc(0);
    });
  });

  return wss;
}

// Add a way to monitor frame processing
if (process.env.NODE_ENV === 'development') {
  setInterval(() => {
    const memory = process.memoryUsage();
    console.log('Memory usage:', {
      heapTotal: Math.round(memory.heapTotal / 1024 / 1024) + 'MB',
      heapUsed: Math.round(memory.heapUsed / 1024 / 1024) + 'MB',
    });
  }, 5000);
}

app.whenReady().then(() => {
  createWindow();
  createWebSocketServer();
});

app.on('window-all-closed', () => {
  if (process.platform !== 'darwin') {
    app.quit();
  }
});

// Handle any uncaught errors
process.on('uncaughtException', (error) => {
  console.error('Uncaught exception:', error);
});

process.on('unhandledRejection', (error) => {
  console.error('Unhandled rejection:', error);
});