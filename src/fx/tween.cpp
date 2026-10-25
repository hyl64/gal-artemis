// tween.cpp - see tween.h

#include "fx/tween.h"

#include <cmath>
#include <cstdlib>

namespace artemis
{
namespace fx
{
	namespace
	{
		struct NamedEase
		{
			const char* name;
			Ease ease;
		};

		// The five the corpus uses, then the regular in/out/inout family.
		const NamedEase kEases[] = {
			{ "none", Ease::Linear },
			{ "linear", Ease::Linear },
			{ "easein_sine", Ease::InSine },
			{ "easeout_sine", Ease::OutSine },
			{ "easeinout_sine", Ease::InOutSine },
			{ "easein_quad", Ease::InQuad },
			{ "easeout_quad", Ease::OutQuad },
			{ "easeinout_quad", Ease::InOutQuad },
			{ "easein_cubic", Ease::InCubic },
			{ "easeout_cubic", Ease::OutCubic },
			{ "easeinout_cubic", Ease::InOutCubic },
			{ "easein_quart", Ease::InQuart },
			{ "easeout_quart", Ease::OutQuart },
			{ "easeinout_quart", Ease::InOutQuart },
			{ "easein_quint", Ease::InQuint },
			{ "easeout_quint", Ease::OutQuint },
			{ "easeinout_quint", Ease::InOutQuint },
		};

		double Pow(double base, int power)
		{
			double r = 1.0;
			for(int i = 0; i < power; i++)
				r *= base;
			return r;
		}

		double InPow(double p, int power) { return Pow(p, power); }
		double OutPow(double p, int power) { return 1.0 - Pow(1.0 - p, power); }
		double InOutPow(double p, int power)
		{
			if(p < 0.5)
				return InPow(p * 2.0, power) / 2.0;
			return 1.0 - InPow(2.0 - p * 2.0, power) / 2.0;
		}

		int64_t ClampI(int64_t v, int64_t low, int64_t high)
		{
			return v < low ? low : (v > high ? high : v);
		}

		// Progress through the tween in [0,1] and whether the pass is running
		// backwards.  `done` freezes the value at the end of the last pass.
		struct Progress
		{
			double p = 0.0;
			bool reverse = false;
			bool done = false;
		};

		Progress ProgressAt(int64_t durationMs, int64_t delayMs, int yoyo, int64_t tMs)
		{
			Progress out;
			const int64_t t = tMs - delayMs;
			if(t <= 0)
				return out; // still in the delay: sit at `from`
			if(durationMs <= 0)
			{
				out.p = 1.0;
				out.done = true;
				return out;
			}
			// yoyo < 0 has no end, so the pass index can be anything; 1+yoyo passes
			// otherwise, and the value freezes after the last one.
			int64_t pass = t / durationMs;
			const int64_t within = t % durationMs;
			if(yoyo == 0)
			{
				if(pass > 0)
				{
					out.p = 1.0;
					out.done = true;
					return out;
				}
				out.p = (double)within / (double)durationMs;
				return out;
			}
			if(yoyo > 0)
			{
				const int64_t total = 1 + (int64_t)yoyo;
				if(pass >= total)
				{
					// the last pass decides where it stops: even index = forward run
					out.reverse = ((total - 1) % 2) == 1;
					out.p = out.reverse ? 0.0 : 1.0;
					out.done = true;
					return out;
				}
			}
			out.reverse = (pass % 2) == 1;
			out.p = (double)within / (double)durationMs;
			return out;
		}

		int64_t Rounded(double v)
		{
			// half away from zero, so -0.5 becomes -1 rather than 0: a symmetric ramp
			// (diff="-255" then diff="255") must land on a symmetric pair of values.
			return (int64_t)(v >= 0.0 ? std::floor(v + 0.5) : std::ceil(v - 0.5));
		}

		// The fraction of the way from `from` to `to`.  A running pass bends the
		// clock through the curve and mirrors it when the pass runs backwards, so
		// the value is continuous at every turn (both sides reach 1 there).  A
		// finished tween freezes on the endpoint its last pass arrived at.
		double UnitAt(const Tween& tween, const Progress& progress)
		{
			if(progress.done)
				return progress.p; // 1.0 = ended on `to`, 0.0 = ended on `from`
			const double eased = EaseUnit(tween.ease, progress.p);
			return progress.reverse ? 1.0 - eased : eased;
		}

		int64_t TweenValue(const Tween& tween, int64_t tMs)
		{
			const Progress progress = ProgressAt(tween.durationMs, tween.delayMs,
				tween.yoyo, tMs);
			const double u = UnitAt(tween, progress);
			const int64_t span = tween.to - tween.from;
			const int64_t low = span < 0 ? tween.to : tween.from;
			const int64_t high = span < 0 ? tween.from : tween.to;
			return ClampI(tween.from + Rounded((double)span * u), low, high);
		}
	}

	bool ParseEase(const std::string& name, Ease& out)
	{
		for(const NamedEase& entry : kEases)
		{
			if(name == entry.name)
			{
				out = entry.ease;
				return true;
			}
		}
		return false;
	}

	const char* EaseName(Ease ease)
	{
		for(const NamedEase& entry : kEases)
		{
			if(entry.ease == ease && std::string(entry.name) != "linear")
				return entry.name;
		}
		return "none";
	}

	double EaseUnit(Ease ease, double p)
	{
		if(p <= 0.0)
			return 0.0;
		if(p >= 1.0)
			return 1.0;
		switch(ease)
		{
		case Ease::Linear: return p;
		case Ease::InSine: return 1.0 - std::cos(p * 3.14159265358979323846 / 2.0);
		case Ease::OutSine: return std::sin(p * 3.14159265358979323846 / 2.0);
		case Ease::InOutSine: return -(std::cos(3.14159265358979323846 * p) - 1.0) / 2.0;
		case Ease::InQuad: return InPow(p, 2);
		case Ease::OutQuad: return OutPow(p, 2);
		case Ease::InOutQuad: return InOutPow(p, 2);
		case Ease::InCubic: return InPow(p, 3);
		case Ease::OutCubic: return OutPow(p, 3);
		case Ease::InOutCubic: return InOutPow(p, 3);
		case Ease::InQuart: return InPow(p, 4);
		case Ease::OutQuart: return OutPow(p, 4);
		case Ease::InOutQuart: return InOutPow(p, 4);
		case Ease::InQuint: return InPow(p, 5);
		case Ease::OutQuint: return OutPow(p, 5);
		case Ease::InOutQuint: return InOutPow(p, 5);
		}
		return p;
	}

	int64_t Tween::totalMs() const
	{
		if(durationMs <= 0)
			return delayMs;
		if(yoyo < 0)
			return -1; // never stops
		return delayMs + durationMs * (1 + (int64_t)yoyo);
	}

	int64_t ValueAt(const Tween& tween, int64_t tMs)
	{
		return TweenValue(tween, tMs);
	}

	void ColorAt(const ColorTween& tween, int64_t tMs, uint8_t out[4])
	{
		const Progress progress = ProgressAt(tween.durationMs, tween.delayMs,
			tween.yoyo, tMs);
		double u = progress.done ? progress.p : EaseUnit(tween.ease, progress.p);
		if(progress.reverse)
			u = 1.0 - u;
		for(int i = 0; i < 4; i++)
		{
			const int span = (int)tween.to[i] - (int)tween.from[i];
			const int low = span < 0 ? tween.to[i] : tween.from[i];
			const int high = span < 0 ? tween.from[i] : tween.to[i];
			int v = tween.from[i] + (int)Rounded((double)span * u);
			v = v < low ? low : (v > high ? high : v);
			out[i] = (uint8_t)v;
		}
	}

	bool ParseIntValue(const std::string& text, int64_t& out)
	{
		if(text.empty())
			return false;
		size_t at = 0;
		bool negative = false;
		if(text[0] == '-' || text[0] == '+')
		{
			negative = text[0] == '-';
			at = 1;
		}
		if(at >= text.size())
			return false;
		int64_t value = 0;
		for(; at < text.size(); at++)
		{
			if(text[at] < '0' || text[at] > '9')
				return false;
			value = value * 10 + (text[at] - '0');
		}
		out = negative ? -value : value;
		return true;
	}

	namespace
	{
		const std::string* FindAttr(const std::vector<AttrNameValue>& attrs,
			const std::string& name)
		{
			for(const AttrNameValue& attr : attrs)
				if(attr.name == name)
					return &attr.value;
			return nullptr;
		}
	}

	bool ParseLytween(const std::vector<AttrNameValue>& attrs, LytweenTag& out)
	{
		out = LytweenTag();
		const std::string* id = FindAttr(attrs, "id");
		const std::string* param = FindAttr(attrs, "param");
		const std::string* from = FindAttr(attrs, "from");
		const std::string* to = FindAttr(attrs, "to");
		const std::string* time = FindAttr(attrs, "time");
		if(id == nullptr || id->empty())
		{
			out.note = "no id";
			return false;
		}
		out.id = *id;
		if(param == nullptr || param->empty())
		{
			out.note = "no param";
			return false;
		}
		out.param = *param;
		if(to == nullptr)
		{
			// the two broken tags in the real scene land here, and the note names
			// the attribute that was probably meant, so the data can be fixed
			const std::string* top = FindAttr(attrs, "top");
			out.note = "no `to`";
			if(top != nullptr)
				out.note += " (the tag has top=\"" + *top + "\" where to= belongs)";
			return false;
		}
		if(from == nullptr)
		{
			out.note = "no `from`";
			return false;
		}
		if(time == nullptr)
		{
			out.note = "no `time`";
			return false;
		}
		int64_t fromValue = 0, toValue = 0, timeValue = 0;
		if(!ParseIntValue(*from, fromValue) || !ParseIntValue(*to, toValue) ||
			!ParseIntValue(*time, timeValue))
		{
			out.note = "non-numeric value (a variable?)";
			return false;
		}
		out.tween.from = fromValue;
		out.tween.to = toValue;
		out.tween.durationMs = timeValue;
		const std::string* ease = FindAttr(attrs, "ease");
		if(ease != nullptr && !ease->empty() && !ParseEase(*ease, out.tween.ease))
		{
			out.note = "unknown ease \"" + *ease + "\"";
			return false;
		}
		const std::string* yoyo = FindAttr(attrs, "yoyo");
		if(yoyo != nullptr)
		{
			int64_t value = 0;
			if(!ParseIntValue(*yoyo, value))
			{
				out.note = "non-numeric yoyo";
				return false;
			}
			out.tween.yoyo = (int)value;
		}
		const std::string* delay = FindAttr(attrs, "delay");
		if(delay != nullptr)
		{
			int64_t value = 0;
			if(ParseIntValue(*delay, value))
				out.tween.delayMs = value;
		}
		const std::string* sync = FindAttr(attrs, "sync");
		if(sync != nullptr)
		{
			int64_t value = 1;
			if(ParseIntValue(*sync, value))
				out.tween.sync = value != 0;
		}
		out.usable = true;
		return true;
	}

	int64_t ClampProperty(int64_t value, int64_t low, int64_t high)
	{
		if(high < low)
		{
			const int64_t swap = low;
			low = high;
			high = swap;
		}
		return value < low ? low : (value > high ? high : value);
	}

	int64_t ScetweenValueAt(const Scetween& tween, int64_t base, int64_t tMs)
	{
		Tween shape;
		shape.from = 0;
		shape.to = tween.diff;
		shape.durationMs = tween.durationMs;
		shape.delayMs = tween.delayMs;
		shape.ease = tween.ease;
		shape.yoyo = 0;
		return base + ValueAt(shape, tMs);
	}
}
} // namespace artemis::fx
