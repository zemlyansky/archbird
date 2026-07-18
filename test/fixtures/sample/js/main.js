import Worker from './worker.js'
import { dep } from '@sample/dep'

export function run(worker = new Worker()) {
  dep()
  worker.onmessage = (event) => {
    switch (event.data.status) {
      case 'done':
        return event.data.value
    }
  }
  worker.postMessage({ type: 'run' })
}
