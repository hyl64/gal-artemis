// guistate.cpp - see guistate.h

#include "fx/guistate.h"

#include <algorithm>

namespace artemis
{
namespace fx
{
	namespace
	{
		int ClampInt(int64_t value, int low, int high)
		{
			if(value < low)
				return low;
			if(value > high)
				return high;
			return (int)value;
		}

		GuiSlot MakeSlot(const char* state, const char* layer, const char* role,
			bool observed, const char* evidence)
		{
			GuiSlot slot;
			slot.state = state;
			slot.layer = layer;
			slot.role = role;
			slot.observed = observed;
			slot.evidence = evidence;
			return slot;
		}

		void AddProperty(GuiSlot& slot, const char* param, int init, int diff,
			bool observed)
		{
			GuiProperty property;
			property.param = param;
			property.init = init;
			property.diff = diff;
			property.target = ResolveGuiTarget(param, init, diff);
			property.observed = observed;
			slot.properties.push_back(property);
		}
	}

	const GuiProperty* GuiSlot::Property(const std::string& param) const
	{
		for(const GuiProperty& property : properties)
			if(property.param == param)
				return &property;
		return nullptr;
	}

	GuiProperty* GuiSlot::Property(const std::string& param)
	{
		for(GuiProperty& property : properties)
			if(property.param == param)
				return &property;
		return nullptr;
	}

	int ResolveGuiTarget(const std::string& param, int init, int diff)
	{
		const int64_t value = (int64_t)init + diff;
		if(param == "alpha")
			return ClampInt(value, 0, 255);
		if(param == "xscale" || param == "yscale")
			return ClampInt(value, 0, 0x7FFFFFFF);
		return ClampInt(value, -0x7FFFFFFF, 0x7FFFFFFF);
	}

	int RestingValue(const std::string& state, const std::string& param)
	{
		if(param == "top" || param == "left")
			return 0; // slides always start from their home position
		if(param == "alpha")
		{
			if(state == "in" || state == "show" || state == "backlog_down_in"
				|| state == "backlog_down_out" || state == "backlog_up_in"
				|| state == "backlog_up_out")
				return 255;
			if(state == "on" || state == "click" || state == "rollout"
				|| state == "dragout")
				return 255;
			// out/hide/rollover/overwhite/overtrans/drag all start invisible
			return 0;
		}
		return 0;
	}

	GuiStateMap::GuiStateMap()
	{
		static const char* observedEvidence =
			"first.iet: four [chgmsg] contexts, 80 active definitions";
		static const char* inferredEvidence =
			"inferred from the state name and layer role; absent from first.iet, "
			"backlog.iet, rclick.iet and system/macro_effect.iet";

		GuiSlot in = MakeSlot("in", "msg.text",
			"scenario message window leaving its shown state", true, observedEvidence);
		AddProperty(in, "alpha", 255, -255, true);
		m_slots.push_back(in);

		GuiSlot out = MakeSlot("out", "msg.text",
			"scenario message window leaving its hidden state", true, observedEvidence);
		AddProperty(out, "alpha", 0, -255, true);
		m_slots.push_back(out);

		GuiSlot show = MakeSlot("show", "msg.text",
			"message window, temporary hide", true, observedEvidence);
		AddProperty(show, "alpha", 255, -255, true);
		m_slots.push_back(show);

		GuiSlot hide = MakeSlot("hide", "msg.text",
			"message window, temporary hide", true, observedEvidence);
		AddProperty(hide, "alpha", 0, -255, true);
		m_slots.push_back(hide);

		GuiSlot on = MakeSlot("on", "ui.button.on",
			"toggle button's ON badge", false, inferredEvidence);
		AddProperty(on, "alpha", 255, -255, false);
		m_slots.push_back(on);

		GuiSlot click = MakeSlot("click", "ui.button.pressed",
			"button pressed frame", false, inferredEvidence);
		AddProperty(click, "alpha", 255, -255, false);
		m_slots.push_back(click);

		GuiSlot rollover = MakeSlot("rollover", "ui.button.hover",
			"pointer entered the button", false, inferredEvidence);
		AddProperty(rollover, "alpha", 0, 255, false);
		m_slots.push_back(rollover);

		GuiSlot rollout = MakeSlot("rollout", "ui.button.hover",
			"pointer left the button", false, inferredEvidence);
		AddProperty(rollout, "alpha", 255, -255, false);
		m_slots.push_back(rollout);

		GuiSlot overwhite = MakeSlot("overwhite", "ui.overlay.white",
			"white flash overlay", false, inferredEvidence);
		AddProperty(overwhite, "alpha", 0, 255, false);
		m_slots.push_back(overwhite);

		GuiSlot overtrans = MakeSlot("overtrans", "ui.overlay.transition",
			"transition overlay", false, inferredEvidence);
		AddProperty(overtrans, "alpha", 0, 255, false);
		m_slots.push_back(overtrans);

		GuiSlot drag = MakeSlot("drag", "ui.drag.layer",
			"drag cursor / handle shown", false, inferredEvidence);
		AddProperty(drag, "alpha", 0, 255, false);
		m_slots.push_back(drag);

		GuiSlot dragout = MakeSlot("dragout", "ui.drag.layer",
			"drag cursor / handle hidden", false, inferredEvidence);
		AddProperty(dragout, "alpha", 255, -255, false);
		m_slots.push_back(dragout);

		GuiSlot upIn = MakeSlot("backlog_up_in", "backlog.page",
			"backlog page arriving from below", true, observedEvidence);
		AddProperty(upIn, "top", 0, 288, true);
		AddProperty(upIn, "alpha", 255, -255, true);
		m_slots.push_back(upIn);

		GuiSlot upOut = MakeSlot("backlog_up_out", "backlog.page",
			"backlog page leaving upward", true, observedEvidence);
		AddProperty(upOut, "top", 0, -288, true);
		AddProperty(upOut, "alpha", 255, -255, true);
		m_slots.push_back(upOut);

		GuiSlot downIn = MakeSlot("backlog_down_in", "backlog.page",
			"backlog page arriving from above", true, observedEvidence);
		AddProperty(downIn, "top", 0, -288, true);
		AddProperty(downIn, "alpha", 255, -255, true);
		m_slots.push_back(downIn);

		GuiSlot downOut = MakeSlot("backlog_down_out", "backlog.page",
			"backlog page leaving downward", true, observedEvidence);
		AddProperty(downOut, "top", 0, 288, true);
		AddProperty(downOut, "alpha", 255, -255, true);
		m_slots.push_back(downOut);
	}

	GuiStateMap GuiStateMap::Default()
	{
		return GuiStateMap();
	}

	const GuiSlot* GuiStateMap::Find(const std::string& state) const
	{
		for(const GuiSlot& slot : m_slots)
			if(slot.state == state)
				return &slot;
		return nullptr;
	}

	GuiSlot* GuiStateMap::Find(const std::string& state)
	{
		for(GuiSlot& slot : m_slots)
			if(slot.state == state)
				return &slot;
		return nullptr;
	}

	bool GuiStateMap::Apply(const GuiDefinition& definition)
	{
		GuiSlot* slot = Find(definition.type);
		if(slot == nullptr)
			return false;
		if(definition.mode == "init")
		{
			const std::string param = definition.param.empty() ? "alpha" : definition.param;
			GuiProperty* property = slot->Property(param);
			if(property == nullptr)
			{
				AddProperty(*slot, param.c_str(), RestingValue(definition.type, param), 0,
					true);
				property = &slot->properties.back();
			}
			property->init = RestingValue(definition.type, param);
			property->diff = 0;
			property->target = property->init;
			property->observed = true;
			slot->observed = true;
			return true;
		}
		if(definition.mode == "add")
		{
			if(definition.param.empty() || !definition.hasDiff)
				return false;
			GuiProperty* property = slot->Property(definition.param);
			if(property == nullptr)
			{
				AddProperty(*slot, definition.param.c_str(),
					RestingValue(definition.type, definition.param), 0, true);
				property = &slot->properties.back();
			}
			property->diff = (int)definition.diff;
			property->target = ResolveGuiTarget(definition.param, property->init,
				property->diff);
			property->observed = true;
			slot->observed = true;
			return true;
		}
		return false;
	}

	bool GuiStateMap::SetSlot(const std::string& state, const std::string& layer,
		const std::string& role)
	{
		GuiSlot* slot = Find(state);
		if(slot == nullptr)
			return false;
		slot->layer = layer;
		slot->role = role;
		return true;
	}

	bool GuiStateMap::SetProperty(const std::string& state, const std::string& param,
		int init, int diff)
	{
		GuiSlot* slot = Find(state);
		if(slot == nullptr)
			return false;
		GuiProperty* property = slot->Property(param);
		if(property == nullptr)
		{
			AddProperty(*slot, param.c_str(), init, diff, false);
			return true;
		}
		property->init = init;
		property->diff = diff;
		property->target = ResolveGuiTarget(param, init, diff);
		return true;
	}

	size_t GuiStateMap::ObservedSlots() const
	{
		size_t count = 0;
		for(const GuiSlot& slot : m_slots)
			if(slot.observed)
				count++;
		return count;
	}

	size_t GuiStateMap::InferredSlots() const
	{
		return m_slots.size() - ObservedSlots();
	}
}
} // namespace artemis::fx
