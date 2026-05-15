import type { CompanionVariableDefinition, CompanionVariableValues } from '@companion-module/base'
import type { CoreVideoState } from './state.js'

export const variableDefinitions: CompanionVariableDefinition[] = [
	{ variableId: 'meeting_state',       name: 'Meeting State' },
	{ variableId: 'active_speaker_name', name: 'Active Speaker Name' },
	{ variableId: 'active_speaker_id',   name: 'Active Speaker ID' },
	{ variableId: 'participant_count',   name: 'Participant Count' },
	{ variableId: 'output_count',        name: 'Output Count' },
]

// Build variable values from the current state.
// Per-output variables (output_1_source, output_1_participant, …) are generated
// dynamically for up to 16 outputs.
export function buildVariableValues(state: CoreVideoState): CompanionVariableValues {
	const vals: CompanionVariableValues = {
		meeting_state:       state.meetingState,
		active_speaker_name: state.activeSpeakerName,
		active_speaker_id:   String(state.activeSpeakerId),
		participant_count:   String(state.participants.length),
		output_count:        String(state.outputs.length),
	}

	state.outputs.forEach((o, i) => {
		const n = i + 1
		vals[`output_${n}_source`]      = o.source
		vals[`output_${n}_participant`]  = o.display_name || String(o.participant_id)
		vals[`output_${n}_mode`]         = o.assignment_mode
	})

	return vals
}

// Dynamic variable definitions for up to 16 outputs.
export function buildOutputVariableDefs(count: number): CompanionVariableDefinition[] {
	const defs: CompanionVariableDefinition[] = []
	for (let i = 1; i <= Math.min(count, 16); i++) {
		defs.push(
			{ variableId: `output_${i}_source`,      name: `Output ${i} Source` },
			{ variableId: `output_${i}_participant`,  name: `Output ${i} Participant` },
			{ variableId: `output_${i}_mode`,         name: `Output ${i} Assignment Mode` },
		)
	}
	return defs
}
