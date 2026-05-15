import { combineRgb } from '@companion-module/base'
import type { CompanionFeedbackDefinitions } from '@companion-module/base'
import type { CoreVideoInstance } from './index.js'
import type { MeetingState } from './state.js'

const GREEN  = combineRgb(0,   196, 79)
const AMBER  = combineRgb(230, 160, 32)
const RED    = combineRgb(204, 32,  32)
const BLUE   = combineRgb(41,  121, 255)
const GREY   = combineRgb(40,  40,  60)
const WHITE  = combineRgb(255, 255, 255)
const BLACK  = combineRgb(0,   0,   0)

const STATE_COLORS: Record<MeetingState, number> = {
	in_meeting: GREEN,
	joining:    AMBER,
	recovering: AMBER,
	leaving:    AMBER,
	failed:     RED,
	idle:       GREY,
}

export function buildFeedbacks(instance: CoreVideoInstance): CompanionFeedbackDefinitions {
	return {
		meeting_state: {
			type: 'boolean',
			name: 'Meeting State',
			description: 'Lights up when the meeting is in the specified state',
			defaultStyle: { bgcolor: GREEN, color: BLACK },
			options: [
				{
					type: 'dropdown',
					id: 'state',
					label: 'State',
					default: 'in_meeting',
					choices: [
						{ id: 'in_meeting', label: '● In Meeting' },
						{ id: 'joining',    label: '⟳ Joining' },
						{ id: 'recovering', label: '⟳ Recovering' },
						{ id: 'leaving',    label: '⟳ Leaving' },
						{ id: 'failed',     label: '✕ Failed' },
						{ id: 'idle',       label: '○ Idle' },
					],
				},
			],
			callback: (feedback) => instance.state.meetingState === feedback.options['state'],
		},

		meeting_state_color: {
			type: 'advanced',
			name: 'Meeting State Color (dynamic)',
			description: 'Changes button color to match the current meeting state',
			options: [],
			callback: () => ({
				bgcolor: STATE_COLORS[instance.state.meetingState] ?? GREY,
				color: WHITE,
				text: instance.state.meetingState.replace('_', ' ').toUpperCase(),
			}),
		},

		active_speaker_id: {
			type: 'boolean',
			name: 'Is Active Speaker (by ID)',
			description: 'Lights up when the specified participant is the active speaker',
			defaultStyle: { bgcolor: BLUE, color: WHITE },
			options: [
				{
					type: 'number', id: 'participant_id',
					label: 'Participant ID', default: 0, min: 0,
				},
			],
			callback: (feedback) =>
				instance.state.activeSpeakerId === (feedback.options['participant_id'] as number),
		},

		output_assigned: {
			type: 'boolean',
			name: 'Output Has Participant',
			description: 'Lights up when the named OBS source has a participant assigned',
			defaultStyle: { bgcolor: BLUE, color: WHITE },
			options: [
				{ type: 'textinput', id: 'source', label: 'OBS Source Name', default: '' },
			],
			callback: (feedback) => {
				const src = feedback.options['source'] as string
				const out = instance.state.outputs.find((o) => o.source === src)
				return !!out && out.participant_id > 0
			},
		},

		output_active_speaker: {
			type: 'boolean',
			name: 'Output Tracking Active Speaker',
			description: 'Lights up when the named OBS source is in active-speaker mode',
			defaultStyle: { bgcolor: AMBER, color: BLACK },
			options: [
				{ type: 'textinput', id: 'source', label: 'OBS Source Name', default: '' },
			],
			callback: (feedback) => {
				const src = feedback.options['source'] as string
				const out = instance.state.outputs.find((o) => o.source === src)
				return !!out && out.active_speaker
			},
		},

		recovery_active: {
			type: 'boolean',
			name: 'Auto-Recovery Active',
			description: 'Lights up while CoreVideo is attempting to reconnect',
			defaultStyle: { bgcolor: AMBER, color: BLACK },
			options: [],
			callback: () => instance.state.meetingState === 'recovering',
		},
	}
}
