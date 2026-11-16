#include "override_prefs.h"

#include <algorithm>
#include <climits>
#include <fstream>
#include <mutex>

#include "lib/json/json.h"
#include "utility.h"

namespace OverridePrefs
{
	namespace
	{
		constexpr auto kPath = R"(Data\NVSE\Plugins\kNVSE_overrides.json)";

		std::mutex s_mutex;
		Prefs s_prefs;
		bool s_changed = false;

		std::vector<std::string> ReadStrings(const nlohmann::json& json, const char* key)
		{
			std::vector<std::string> strings;
			if (const auto iter = json.find(key); iter != json.end() && iter->is_array())
			{
				for (const auto& element : *iter)
				{
					if (element.is_string())
						strings.push_back(ToLower(element.get<std::string>()));
				}
			}
			return strings;
		}

		std::vector<std::string> Sorted(const std::unordered_set<std::string>& strings)
		{
			std::vector<std::string> sorted(strings.begin(), strings.end());
			std::ranges::sort(sorted);
			return sorted;
		}

		void Set(std::unordered_set<std::string>& strings, std::string_view text, bool contained)
		{
			auto key = ToLower(text);
			if (contained ? strings.insert(std::move(key)).second : strings.erase(key) != 0)
				s_changed = true;
		}

		// Drops the form's entry once it holds no choices. Called under the lock.
		template <typename F>
		void ChangeForm(std::string_view formKey, F&& change)
		{
			const auto key = ToLower(formKey);
			auto iter = s_prefs.forms.try_emplace(key).first;
			change(iter->second);
			if (iter->second.IsEmpty())
				s_prefs.forms.erase(iter);
		}
	}

	void Load()
	{
		Prefs prefs;
		try
		{
			std::ifstream file(kPath);
			if (file.is_open() && file.peek() != std::ifstream::traits_type::eof())
			{
				nlohmann::json json;
				file >> json;
				for (auto& name : ReadStrings(json, "disabled"))
					prefs.disabledOrigins.insert(std::move(name));
				for (auto& path : ReadStrings(json, "disabledFiles"))
					prefs.disabledFiles.insert(std::move(path));
				prefs.order = ReadStrings(json, "order");
				if (const auto forms = json.find("forms"); forms != json.end() && forms->is_object())
				{
					for (const auto& [key, value] : forms->items())
					{
						if (!value.is_object())
							continue;
						FormPrefs form;
						form.order = ReadStrings(value, "order");
						for (auto& name : ReadStrings(value, "disabled"))
							form.disabledOrigins.insert(std::move(name));
						if (!form.IsEmpty())
							prefs.forms.emplace(ToLower(key), std::move(form));
					}
				}
			}
		}
		catch (const std::exception& e)
		{
			ERROR_LOG(FormatString("%s could not be read, the AnimGroupOverride choices made in the kNVSE menu are not applied: %s", kPath, e.what()));
		}
		std::scoped_lock lock(s_mutex);
		// Choices made in the menu and not saved yet are newer. This happens when kNVSEReset is called from the menu's
		// Reload button.
		if (s_changed)
			return;
		s_prefs = std::move(prefs);
	}

	bool Save()
	{
		nlohmann::json json;
		{
			std::scoped_lock lock(s_mutex);
			json["disabled"] = Sorted(s_prefs.disabledOrigins);
			json["disabledFiles"] = Sorted(s_prefs.disabledFiles);
			json["order"] = s_prefs.order;
			auto forms = nlohmann::json::object();
			for (const auto& [key, form] : s_prefs.forms)
			{
				auto& entry = forms[key];
				if (!form.order.empty())
					entry["order"] = form.order;
				if (!form.disabledOrigins.empty())
					entry["disabled"] = Sorted(form.disabledOrigins);
			}
			json["forms"] = std::move(forms);
			s_changed = false;
		}
		std::ofstream file(kPath, std::ios::trunc);
		file << json.dump(2) << '\n';
		if (!file)
		{
			ERROR_LOG(FormatString("Failed to write %s", kPath));
			return false;
		}
		return true;
	}

	bool HasChanges()
	{
		std::scoped_lock lock(s_mutex);
		return s_changed;
	}

	Prefs Get()
	{
		std::scoped_lock lock(s_mutex);
		return s_prefs;
	}

	bool IsOriginDisabled(std::string_view name)
	{
		const auto key = ToLower(name);
		std::scoped_lock lock(s_mutex);
		return s_prefs.disabledOrigins.contains(key);
	}

	bool IsFileDisabled(std::string_view path)
	{
		const auto key = ToLower(path);
		std::scoped_lock lock(s_mutex);
		return s_prefs.disabledFiles.contains(key);
	}

	bool HasFormPrefs()
	{
		std::scoped_lock lock(s_mutex);
		return !s_prefs.forms.empty();
	}

	FormPrefs GetForm(std::string_view formKey)
	{
		const auto key = ToLower(formKey);
		std::scoped_lock lock(s_mutex);
		const auto iter = s_prefs.forms.find(key);
		return iter != s_prefs.forms.end() ? iter->second : FormPrefs{};
	}

	int GetRank(const std::vector<std::string>& order, std::string_view name)
	{
		if (order.empty())
			return 0;
		const auto key = ToLower(name);
		const auto iter = std::ranges::find(order, key);
		if (iter == order.end())
			return INT_MAX;
		return static_cast<int>(order.end() - iter);
	}

	void SetOriginDisabled(std::string_view name, bool disabled)
	{
		std::scoped_lock lock(s_mutex);
		Set(s_prefs.disabledOrigins, name, disabled);
	}

	void SetFileDisabled(std::string_view path, bool disabled)
	{
		std::scoped_lock lock(s_mutex);
		Set(s_prefs.disabledFiles, path, disabled);
	}

	void SetOrder(std::vector<std::string> order)
	{
		for (auto& name : order)
			name = ToLower(name);
		std::scoped_lock lock(s_mutex);
		if (order != s_prefs.order)
		{
			s_prefs.order = std::move(order);
			s_changed = true;
		}
	}

	void SetFormOrder(std::string_view formKey, std::vector<std::string> order)
	{
		for (auto& name : order)
			name = ToLower(name);
		std::scoped_lock lock(s_mutex);
		ChangeForm(formKey, [&](FormPrefs& form)
		{
			if (order != form.order)
			{
				form.order = std::move(order);
				s_changed = true;
			}
		});
	}

	void SetFormOriginDisabled(std::string_view formKey, std::string_view name, bool disabled)
	{
		std::scoped_lock lock(s_mutex);
		ChangeForm(formKey, [&](FormPrefs& form) { Set(form.disabledOrigins, name, disabled); });
	}

	void ResetForm(std::string_view formKey)
	{
		const auto key = ToLower(formKey);
		std::scoped_lock lock(s_mutex);
		if (s_prefs.forms.erase(key))
			s_changed = true;
	}
}
