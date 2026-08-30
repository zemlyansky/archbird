export function tsFlow(values: number[], enabled: boolean): number {
  for (const value of values) {
    if (value > 0 && enabled)
      return value;
  }
  return enabled ? 1 : 0;
}
