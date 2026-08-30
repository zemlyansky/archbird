export const View = ({ready}: {ready: boolean}) => {
  if (ready)
    return ready ? <div>ready</div> : <div>waiting</div>;
  return <div>waiting</div>;
};
