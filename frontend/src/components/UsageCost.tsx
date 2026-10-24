/** Aggregate cost is an observed subtotal; missing usage is never free work. */
export function UsageCost({ subtotal, unknownCalls, complete }: {
  subtotal: number; unknownCalls?: number; complete?: boolean;
}) {
  const amount = `$${subtotal.toFixed(4)}`;
  if (complete === true && unknownCalls === 0) return <>{amount}</>;
  return <span style={{ overflowWrap: 'anywhere', minWidth: 0 }}>{amount} observed; {unknownCalls && unknownCalls > 0
    ? `${unknownCalls} call${unknownCalls === 1 ? '' : 's'} with unknown usage`
    : 'total unavailable'}</span>;
}
