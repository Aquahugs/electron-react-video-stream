import React, { useEffect, useRef, useState } from 'react';

function App() {
  const canvasRef = useRef(null);
  const [dimensions] = useState({ width: 710, height: 1080 });
  const frameCountRef = useRef(0);
  const lastTimeRef = useRef(Date.now());

  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) return;

    const ctx = canvas.getContext('2d', { 
      alpha: false,
      desynchronized: true
    });
    
    canvas.width = dimensions.width;
    canvas.height = dimensions.height;

    if (window.electron?.on) {
      window.electron.on('frame-data', (frameData) => {
        try {
          // Direct RGBA frame handling
          const imageData = new ImageData(
            new Uint8ClampedArray(frameData),
            dimensions.width,
            dimensions.height
          );
          
          ctx.putImageData(imageData, 0, 0);
          
          // Update FPS counter
          frameCountRef.current++;
          const now = Date.now();
          if (now - lastTimeRef.current >= 1000) {
            const fps = frameCountRef.current;
            console.log(`FPS: ${fps}`);
            frameCountRef.current = 0;
            lastTimeRef.current = now;
          }
        } catch (error) {
          console.error('Error processing frame:', error);
        }
      });
    }
  }, [dimensions]);

  return (
    <div className="flex flex-col items-center justify-center min-h-screen bg-gray-900 p-4">
      <h1 className="text-2xl font-bold mb-4 text-white">iPad Stream</h1>
      <div className="relative">
        <canvas 
          ref={canvasRef}
          className="border border-gray-700 rounded-lg shadow-lg"
          style={{ 
            width: `${dimensions.width}px`,
            height: `${dimensions.height}px`,
            imageRendering: 'auto'
          }}
        />
      </div>
    </div>
  );
}

export default App;