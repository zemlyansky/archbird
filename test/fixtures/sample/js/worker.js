self.onmessage = (event) => {
  switch (event.data.type) {
    case 'run':
      self.postMessage({ status: 'done', value: 3 })
      break
  }
}
