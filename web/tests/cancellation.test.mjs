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

test("a stale read error does not reach either callback", async () => {
  const old = deferredFile("old.png");
  let currentId = 1;
  const pending = runForCurrentRequest(old, 1, (id) => id === currentId,
    () => assert.fail("stale job dispatched"), () => assert.fail("stale error delivered"));
  currentId = 2;
  old.reject(new Error("old read failed"));
  assert.equal(await pending, false);
});

test("a current file read delivers its buffer once", async () => {
  const file = deferredFile("current.png");
  const payload = new ArrayBuffer(3);
  const buffers = [];
  const pending = runForCurrentRequest(file, 7, (id) => id === 7,
    (buffer) => buffers.push(buffer), () => assert.fail("current read failed"));
  file.resolve(payload);
  assert.equal(await pending, true);
  assert.deepEqual(buffers, [payload]);
});
