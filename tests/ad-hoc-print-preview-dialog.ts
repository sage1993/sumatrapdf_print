// UI smoke test for the Sumatra-owned print preview dialog.

import { tmpPath, EXE, cmdId, runStandalone } from "./util.ts";
import { withControlledSumatra } from "./control.ts";
import { writeMultiPagePdf } from "./print-util.ts";
import {
  enumWindows,
  captureWindowToPng,
  findChildWindow,
  getClassName,
  getWindowPid,
  getWindowText,
  getWindowRect,
  isWindowVisible,
  moveWindow,
  postMessage,
  sendMessage,
  sleep,
  waitForWindowIdle,
  WM_COMMAND,
} from "./winapi.ts";
import { pressEscape, sendCommand, waitForFrame } from "./win-automation.ts";

const BN_CLICKED = 0;
const IDC_PP_ADVANCED = 1087;
const IDC_PP_NEXT = 1111;
const IDC_PP_PREV = 1110;
const IDC_PP_ORIENT_AUTO = 1100;
const IDC_PP_ORIENT_LANDSCAPE = 1102;
const IDC_PP_PAGE_SETUP = 1113;

function findPrintDialog(pid: number, frame: number): number {
  let result = 0;
  enumWindows((hwnd) => {
    if (hwnd !== frame && getWindowPid(hwnd) === pid && getClassName(hwnd) === "#32770" && isWindowVisible(hwnd)) {
      if (getWindowText(hwnd) === "Print") {
        result = hwnd;
        return false;
      }
    }
    return true;
  });
  return result;
}

function findDialog(pid: number, title: string): number {
  let result = 0;
  enumWindows((hwnd) => {
    if (getWindowPid(hwnd) === pid && getClassName(hwnd) === "#32770" && isWindowVisible(hwnd)) {
      if (getWindowText(hwnd) === title) {
        result = hwnd;
        return false;
      }
    }
    return true;
  });
  return result;
}

async function waitForPrintDialog(pid: number, frame: number): Promise<number> {
  const deadline = Date.now() + 5000;
  for (;;) {
    const dialog = findPrintDialog(pid, frame);
    if (dialog) {
      return dialog;
    }
    if (Date.now() >= deadline) {
      throw new Error("print-preview-dialog: custom Print dialog did not open");
    }
    await sleep(50);
  }
}

async function waitForDialog(pid: number, title: string): Promise<number> {
  const deadline = Date.now() + 5000;
  for (;;) {
    const dialog = findDialog(pid, title);
    if (dialog) {
      return dialog;
    }
    if (Date.now() >= deadline) {
      throw new Error(`print-preview-dialog: '${title}' dialog did not open`);
    }
    await sleep(50);
  }
}

function clickDialogButton(dialog: number, id: number): void {
  sendMessage(dialog, WM_COMMAND, id | (BN_CLICKED << 16), 0);
}

async function waitForPreview(dialog: number): Promise<void> {
  if (!(await waitForWindowIdle(dialog, 5000))) {
    throw new Error("print-preview-dialog: preview did not settle");
  }
}

export async function testit(): Promise<void> {
  const pdf = tmpPath("print-preview-dialog.pdf");
  writeMultiPagePdf(pdf, ["one", "two", "three"]);

  await withControlledSumatra(
    EXE,
    async (client, proc) => {
      const frame = await waitForFrame(proc.pid!);
      await client.waitForRenderIdle(30000);
      await client.setNotificationsEnabled(false);

      sendCommand(frame, cmdId("CmdPrint"));
      const dialog = await waitForPrintDialog(proc.pid!, frame);
      if (!findChildWindow(dialog, "ComboBox")) {
        throw new Error("print-preview-dialog: printer combo box is missing");
      }
      if (!findChildWindow(dialog, "Edit")) {
        throw new Error("print-preview-dialog: dialog controls are missing");
      }

      await waitForPreview(dialog);
      const screenshot = tmpPath("print-preview-dialog.png");
      if (!captureWindowToPng(dialog, screenshot)) {
        throw new Error("print-preview-dialog: could not capture dialog");
      }

      clickDialogButton(dialog, IDC_PP_NEXT);
      await waitForPreview(dialog);
      clickDialogButton(dialog, IDC_PP_PREV);
      clickDialogButton(dialog, IDC_PP_ORIENT_LANDSCAPE);
      clickDialogButton(dialog, IDC_PP_ORIENT_AUTO);

      const rect = getWindowRect(dialog);
      moveWindow(dialog, rect.x, rect.y, rect.dx + 80, rect.dy + 50);
      moveWindow(dialog, rect.x, rect.y, rect.dx, rect.dy);

      if (!postMessage(dialog, WM_COMMAND, IDC_PP_PAGE_SETUP, 0)) {
        throw new Error("print-preview-dialog: could not open Page Setup");
      }
      const pageSetup = await waitForDialog(proc.pid!, "Page Setup");
      await pressEscape(pageSetup);

      if (!postMessage(dialog, WM_COMMAND, IDC_PP_ADVANCED, 0)) {
        throw new Error("print-preview-dialog: could not open Advanced");
      }
      const advanced = await waitForDialog(proc.pid!, "Advanced");
      await pressEscape(advanced);

      await pressEscape(dialog);
    },
    [pdf],
  );
}

if (import.meta.main) {
  await runStandalone(testit);
}
