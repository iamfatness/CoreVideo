export type MeetingState = 'idle' | 'joining' | 'in_meeting' | 'leaving' | 'recovering' | 'failed'

export interface Participant {
	id: number
	name: string
	has_video: boolean
	is_talking: boolean
	is_muted: boolean
}

export interface Output {
	source: string
	display_name: string
	participant_id: number
	active_speaker: boolean
	assignment_mode: 'participant' | 'active_speaker' | 'spotlight' | 'screen_share'
	spotlight_slot: number
	isolate_audio: boolean
	audio_channels: 'mono' | 'stereo'
}

export interface CoreVideoState {
	meetingState: MeetingState
	activeSpeakerId: number
	activeSpeakerName: string
	participants: Participant[]
	outputs: Output[]
}

export function defaultState(): CoreVideoState {
	return {
		meetingState: 'idle',
		activeSpeakerId: 0,
		activeSpeakerName: '',
		participants: [],
		outputs: [],
	}
}
