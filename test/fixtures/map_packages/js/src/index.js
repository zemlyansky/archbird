export { feature } from "./feature.js";
export const value = 1;

export function dispatch(worker, event) {
  worker.postMessage({type: "run"});
  return event.data.status === "done";
}
