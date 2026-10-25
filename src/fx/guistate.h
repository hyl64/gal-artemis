// guistate.h - what a `[scetween type="..."]` state slot actually points at
//
// first.iet defines the GUI transitions, not the tween curve: four `[chgmsg]`
// contexts repeat the same set, and every definition is a relative delta on a
// layer property:
//
//   [scetween mode="init" type="in"]
//   [scetween mode="add"  type="in" param="alpha" diff="-255" time="400" delay="20"]
//   [scetween mode="add"  type="backlog_down_in" param="top" diff="-288" ...]
//
// Counting the real file: 88 scetween lines, of which **80 are active** and 8
// are commented out (`//[scetween ...]`).  The signatures are:
//
//   type               active lines  properties (init -> init+diff)
//   in / out                  8      alpha 255->0 / 0->0
//   show / hide               8      alpha 255->0 / 0->0
//   backlog_down_in          12      top 0->-288, alpha 255->0
//   backlog_down_out         12      top 0->+288, alpha 255->0
//   backlog_up_in            12      top 0->+288, alpha 255->0
//   backlog_up_out           12      top 0->-288, alpha 255->0
//
// The `top`/`left` variants for in/out are the commented lines, which is why
// only alpha is marked observed for those two.  Three contexts write `time=`
// and the fourth writes `msec=` for the same field - the parser accepts both.
//
// The task listed fifteen GUI slots but actually names sixteen; the corpus
// contains eight of them.  So the table has all sixteen, eight marked observed
// (driven by the real definitions above) and eight marked inferred from the
// state name and the layer it must belong to:
//
//   on, click, rollover, rollout, overwhite, overtrans, drag, dragout
//
// Every row is configurable (SetSlot/SetProperty), because these are logical
// layer roles, not stage layer ids: the renderer maps `msg.text`, `backlog.page`
// or `ui.overlay.white` to whatever the title's layout file calls them.

#ifndef ARTEMIS_FX_GUISTATE_H
#define ARTEMIS_FX_GUISTATE_H

#include <cstdint>
#include <string>
#include <vector>

namespace artemis
{
namespace fx
{
	struct GuiProperty
	{
		std::string param; // alpha, top, left
		int init = 0;      // the value mode="init" leaves the slot at
		int diff = 0;      // the real delta
		int target = 0;    // clamp(init + diff) for the property's legal range
		bool observed = false; // true when a real definition produced these numbers
	};

	struct GuiSlot
	{
		std::string state; // the `type` attribute
		std::string layer; // logical layer role
		std::string role;  // one line a renderer can read
		bool observed = false;
		std::string evidence;
		std::vector<GuiProperty> properties;

		const GuiProperty* Property(const std::string& param) const;
		GuiProperty* Property(const std::string& param);
	};

	// One real `[scetween ...]` command, as parsed by the caller.
	struct GuiDefinition
	{
		std::string mode;    // init / add
		std::string type;    // the state slot
		std::string param;   // alpha / top / left (empty for init)
		int64_t diff = 0;
		bool hasDiff = false;
		int64_t timeMs = 0;
		std::string timeSpelling; // "time" or "msec" - both occur
		std::string delay;
		std::string ease;
	};

	// Clamp a property to its legal range: alpha 0..255, positions unbounded.
	int ResolveGuiTarget(const std::string& param, int init, int diff);

	// The value a state's slot rests at when mode="init" runs, before any delta.
	int RestingValue(const std::string& state, const std::string& param);

	class GuiStateMap
	{
	public:
		GuiStateMap(); // the sixteen-slot table

		static GuiStateMap Default();

		const std::vector<GuiSlot>& Slots() const { return m_slots; }
		std::vector<GuiSlot>& Slots() { return m_slots; }
		const GuiSlot* Find(const std::string& state) const;
		GuiSlot* Find(const std::string& state);

		// Merge one real definition.  Returns false for an unknown state, a
		// missing mode, or an `add` without a diff - the caller counts the miss.
		bool Apply(const GuiDefinition& definition);

		// Configuration: retarget a slot to another layer role, or override one
		// property (a title whose GUI names things differently does this once).
		bool SetSlot(const std::string& state, const std::string& layer,
			const std::string& role);
		bool SetProperty(const std::string& state, const std::string& param, int init,
			int diff);

		size_t ObservedSlots() const;
		size_t InferredSlots() const;

	private:
		std::vector<GuiSlot> m_slots;
	};
}
} // namespace artemis::fx

#endif // ARTEMIS_FX_GUISTATE_H
