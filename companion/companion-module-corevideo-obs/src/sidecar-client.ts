import * as net from 'net'
import type { CoreVideoInstance } from './index.js'

export class SidecarClient {
	private socket: net.Socket | null = null
	private recvBuffer = ''
	private reconnectTimer: ReturnType<typeof setTimeout> | null = null
	private destroyed = false
	private host = ''
	private port = 19880

	constructor(private readonly instance: CoreVideoInstance) {}

	connect(host: string, port: number): void {
		if (this.destroyed) return
		this.host = host
		this.port = port
		this.socket?.destroy()
		this.socket = net.createConnection({ host, port })
		this.socket.setKeepAlive(true, 15000)
		this.socket.setEncoding('utf8')
		this.recvBuffer = ''

		this.socket.on('connect', () => {
			this.instance.log('info', `Sidecar connected (${host}:${port})`)
			this.instance.onSidecarConnected()
			this.send({ cmd: 'subscribe_events' })
			this.send({ cmd: 'status' })
			this.send({ cmd: 'list_templates' })
		})

		this.socket.on('data', (chunk: string) => {
			this.recvBuffer += chunk
			const lines = this.recvBuffer.split('\n')
			this.recvBuffer = lines.pop() ?? ''
			for (const line of lines) {
				const t = line.trim()
				if (t) this.instance.onSidecarMessage(t)
			}
		})

		this.socket.on('error', (err) => {
			this.instance.log('debug', `Sidecar error: ${err.message}`)
		})

		this.socket.on('close', () => {
			this.instance.onSidecarDisconnected()
			if (!this.destroyed) this.scheduleReconnect()
		})
	}

	send(cmd: Record<string, unknown>): void {
		if (!this.socket?.writable) return
		try {
			this.socket.write(JSON.stringify(cmd) + '\n')
		} catch {
			/* socket may have just closed */
		}
	}

	private scheduleReconnect(): void {
		if (this.destroyed || this.reconnectTimer) return
		this.reconnectTimer = setTimeout(() => {
			this.reconnectTimer = null
			if (!this.destroyed) this.connect(this.host, this.port)
		}, 5000)
	}

	destroy(): void {
		this.destroyed = true
		if (this.reconnectTimer) {
			clearTimeout(this.reconnectTimer)
			this.reconnectTimer = null
		}
		this.socket?.destroy()
		this.socket = null
	}
}
