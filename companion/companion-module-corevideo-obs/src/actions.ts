import type { CompanionActionDefinitions } from '@companion-module/base'
import type { CoreVideoInstance } from './index.js'

export function buildActions(instance: CoreVideoInstance): CompanionActionDefinitions {
	return {
		join: {
			name: 'Join Meeting',
			options: [
				{ type: 'textinput', id: 'meeting_id', label: 'Meeting ID or Zoom URL', default: '' },
				{ type: 'textinput', id: 'passcode',   label: 'Passcode (optional)',    default: '' },
				{ type: 'textinput', id: 'display_name', label: 'Display Name',         default: 'OBS' },
			],
			callback: (action) => {
				instance.sendCmd({
					cmd: 'join',
					meeting_id:   action.options.meeting_id,
					passcode:     action.options.passcode,
					display_name: action.options.display_name,
				})
			},
		},

		leave: {
			name: 'Leave Meeting',
			options: [],
			callback: () => instance.sendCmd({ cmd: 'leave' }),
		},

		assign_participant: {
			name: 'Assign Participant to Output',
			options: [
				{
					type: 'textinput', id: 'source',
					label: 'OBS Source Name',
					default: '',
					tooltip: 'Exact name of the Zoom source in OBS',
				},
				{
					type: 'number', id: 'participant_id',
					label: 'Participant ID (0 = active speaker)',
					default: 0, min: 0,
				},
				{
					type: 'checkbox', id: 'active_speaker',
					label: 'Track Active Speaker',
					default: false,
				},
				{
					type: 'checkbox', id: 'isolate_audio',
					label: 'Isolate Audio',
					default: false,
				},
			],
			callback: (action) => {
				instance.sendCmd({
					cmd:            'assign_output',
					source:         action.options.source,
					participant_id: action.options.participant_id,
					active_speaker: action.options.active_speaker,
					isolate_audio:  action.options.isolate_audio,
				})
			},
		},

		assign_spotlight: {
			name: 'Assign Spotlight Slot to Output',
			options: [
				{ type: 'textinput', id: 'source', label: 'OBS Source Name', default: '' },
				{ type: 'number', id: 'slot', label: 'Spotlight Slot (1-based)', default: 1, min: 1, max: 49 },
			],
			callback: (action) => {
				instance.sendCmd({
					cmd:            'assign_output_ex',
					source:         action.options.source,
					mode:           'spotlight',
					spotlight_slot: action.options.slot,
				})
			},
		},

		assign_screen_share: {
			name: 'Assign Screen Share to Output',
			options: [
				{ type: 'textinput', id: 'source', label: 'OBS Source Name', default: '' },
			],
			callback: (action) => {
				instance.sendCmd({
					cmd:    'assign_output_ex',
					source: action.options.source,
					mode:   'screen_share',
				})
			},
		},

		cancel_recovery: {
			name: 'Cancel Auto-Recovery',
			options: [],
			callback: () => instance.sendCmd({ cmd: 'recovery_cancel' }),
		},

		refresh_status: {
			name: 'Refresh Status',
			options: [],
			callback: () => {
				instance.sendCmd({ cmd: 'status' })
				instance.sendCmd({ cmd: 'list_outputs' })
				instance.sendCmd({ cmd: 'list_participants' })
			},
		},
	}
}
