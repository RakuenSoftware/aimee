import { StrictMode } from 'react';
import { createRoot } from 'react-dom/client';
import { BrowserRouter } from 'react-router-dom';
import { ToastProvider } from '@rakuensoftware/smoothgui';
import '@rakuensoftware/smoothgui/styles';
import ConsoleApp from './ConsoleApp';

createRoot(document.getElementById('root')!).render(
  <StrictMode>
    <BrowserRouter>
      <ToastProvider>
        <ConsoleApp />
      </ToastProvider>
    </BrowserRouter>
  </StrictMode>,
);
