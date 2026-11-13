import test from "node:test";
import assert from "node:assert/strict";
import { runForCurrentRequest } from "../src/request-guard.js";

function deferredFile(name) {
  let resolve;
  let reject;
  const promise = new Promise((yes, no) => { resolve = yes; reject = no; });
  return {
    name,
    arrayBuffer: () => promise,
    resolve: (value = new ArrayBuffer(1)) => resolve(value),
    reject,
  };
}

test("stop before a file read resolves schedules no worker job", async () => {
  const file = deferredFile("input.png");
  let currentId = 1;
  const jobs = [];
  const pending = runForCurrentRequest(file, 1, (id) => id === currentId,
    (buffer) => jobs.push({ type: "convert", buffer }), () => assert.fail("stale error"));
  currentId += 1; // stop invalidates the request before terminating its worker
  file.resolve();
  assert.equal(await pending, false);
  assert.deepEqual(jobs, []);
});

test("reverse-order inspections only dispatch the latest selected file", async () => {
  const first = deferredFile("first.png");
  const second = deferredFile("second.png");
  let currentId = 1;
  const jobs = [];
  const inspect = (file, id) => runForCurrentRequest(file, id, (candidate) => candidate === currentId,
    (buffer) => jobs.push({ type: "inspect", name: file.name, buffer }), () => assert.fail("unexpected read error"));
  const oldRead = inspect(first, 1);
  currentId = 2;
  const newRead = inspect(second, 2);
  second.resolve(new ArrayBuffer(2));
  first.resolve(new ArrayBuffer(1));
  await Promise.all([oldRead, newRead]);
  assert.deepEqual(jobs.map(({ name }) => name), ["second.png"]);
});

test("a late old read error cannot replace current status or busy state", async () => {
  const old = deferredFile("old.png");
  let currentId = 1;
  let status = "Inspecting current file";
  let busy = false;
  const pending = runForCurrentRequest(old, 1, (id) => id === currentId,
    () => assert.fail("stale job dispatched"), (error) => { status = error.message; busy = false; });
  currentId = 2;
  busy = true; // newer operation owns the UI now
  old.reject(new Error("old read failed"));
  assert.equal(await pending, false);
  assert.equal(status, "Inspecting current file");
  assert.equal(busy, true);
});

test("current reads dispatch once and preserve the caller's option snapshot", async () => {
  const file = deferredFile("current.png");
  const options = { format: "png", encodingValue: 4 };
  const jobs = [];
  const pending = runForCurrentRequest(file, 7, (id) => id === 7,
    (buffer) => jobs.push({ ...options, buffer }), () => assert.fail("current read failed"));
  file.resolve(new ArrayBuffer(3));
  assert.equal(await pending, true);
  assert.equal(jobs.length, 1);
  assert.equal(jobs[0].format, "png");
  assert.equal(jobs[0].encodingValue, 4);
  assert.equal(jobs[0].buffer.byteLength, 3);
});
