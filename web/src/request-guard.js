// Run asynchronous file reads only while their owning request is current.
export async function runForCurrentRequest(file, requestId, isCurrent, onBuffer, onError) {
  try {
    const buffer = await file.arrayBuffer();
    if (!isCurrent(requestId)) return false;
    onBuffer(buffer);
    return true;
  } catch (error) {
    if (isCurrent(requestId)) onError(error);
    return false;
  }
}
