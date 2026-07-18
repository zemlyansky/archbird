export function handle(event) {
  if (event.data.type === "run") postMessage({status: "done"});
}
