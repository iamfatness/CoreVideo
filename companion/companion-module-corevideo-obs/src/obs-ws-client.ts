import { createHash } from 'crypto'
import WebSocket from 'ws'
import type { CoreVideoInstance } from './index.js'

function computeAuth(password: string, salt: string, challenge: string): string {
	const secret = createHash('sha256').update(password + salt).digest('base64')
	return createHash('sha256').update(secret + challenge).digest('base64')
}

// EventSubscription flags: General(1) | Scenes(4) | Outputs(64)
const EVENT_SUBS = 1 | 4 | 64

export class OBSWebSocketClient {
	private ws: WebSocket | null = null
	private pending = new Map<string, { resolve: (d: unknown) => void; reject: (e: Error) => void }>()
	private reqId = 1
	private reconnectTimer: ReturnType<typeof setTimeout> | null = null
	private destroyed = false
	private host = ''
	private port = 4455
	private password = ''

	constructor(private readonly instance: CoreVideoInstance) {}

	connect(host: string, port: number, password: string): void {
		if (this.destroyed) return
		this.host = host
		this.port = port
		this.password = password
		this.ws?.terminate()
		this.ws = new WebSocket(`ws://${host}:${port}`)

		this.ws.on('open', () => {
			// Hello arrives from server first; nothing to send yet
		})

		this.ws.on('message', (raw) => {
			try {
				this.dispatch(JSON.parse(raw.toString()) as { op: number; d: Record<string, unknown> })
			} catch {
				/* ignore malformed */
			}
		})

		this.ws.on('error', (err) => {
			this.instance.log('debug', `OBS WS error: ${err.message}`)
		})

		this.ws.on('close', () => {
			this.instance.onOBSDisconnected()
			this.scheduleReconnect()
		})
	}

	private dispatch(msg: { op: number; d: Record<string, unknown> }): void {
		switch (msg.op) {
			case 0: {
				// Hello — send Identify
				const authInfo = msg.d['authentication'] as { salt: string; challenge: string } | undefined
				const auth =
					authInfo && this.password
						? computeAuth(this.password, authInfo.salt, authInfo.challenge)
						: undefined
				this.send(1, {
					rpcVersion: 1,
					...(auth ? { authentication: auth } : {}),
					eventSubscriptions: EVENT_SUBS,
				})
				break
			}
			case 2: // Identified
				this.instance.log('info', `OBS WebSocket connected (${this.host}:${this.port})`)
				this.instance.onOBSConnected()
				break

			case 5: {
				// Event
				const ev = msg.d as { eventType: string; eventData?: Record<string, unknown> }
				this.instance.onOBSEvent(ev.eventType, ev.eventData ?? {})
				break
			}
			case 7: {
				// RequestResponse
				const id = msg.d['requestId'] as string
				const req = this.pending.get(id)
				if (req) {
					this.pending.delete(id)
					const status = msg.d['requestStatus'] as { result: boolean; code: number }
					if (status.result) req.resolve(msg.d['responseData'])
					else req.reject(new Error(`OBS error ${status.code}`))
				}
				break
			}
		}
	}

	request<T = unknown>(type: string, data?: Record<string, unknown>): Promise<T> {
		return new Promise((resolve, reject) => {
			if (!this.ws || this.ws.readyState !== WebSocket.OPEN) {
				reject(new Error('OBS not connected'))
				return
			}
			const id = String(this.reqId++)
			this.pending.set(id, { resolve: resolve as (d: unknown) => void, reject })
			this.send(6, { requestType: type, requestId: id, ...(data ? { requestData: data } : {}) })
		})
	}

	private send(op: number, d: Record<string, unknown>): void {
		if (this.ws?.readyState === WebSocket.OPEN) {
			this.ws.send(JSON.stringify({ op, d }))
		}
	}

	private scheduleReconnect(): void {
		if (this.destroyed || this.reconnectTimer) return
		this.reconnectTimer = setTimeout(() => {
			this.reconnectTimer = null
			if (!this.destroyed) this.connect(this.host, this.port, this.password)
		}, 5000)
	}

	destroy(): void {
		this.destroyed = true
		if (this.reconnectTimer) {
			clearTimeout(this.reconnectTimer)
			this.reconnectTimer = null
		}
		this.ws?.terminate()
		this.ws = null
	}
}
