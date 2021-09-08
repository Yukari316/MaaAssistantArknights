#include "InfrastStationTask.h"

#include <thread>
#include <future>
#include <algorithm>
#include <future>
#include <list>

#include <opencv2/opencv.hpp>

#include "Configer.h"
#include "InfrastConfiger.h"
#include "WinMacro.h"
#include "Identify.h"
#include "AsstAux.h"

using namespace asst;

asst::InfrastStationTask::InfrastStationTask(AsstCallback callback, void* callback_arg)
	: IdentifyOperTask(callback, callback_arg)
{
	m_cropped_height_ratio = 0.052;
	m_cropped_upper_y_ratio = 0.441;
	m_cropped_lower_y_ratio = 0.831;
}

bool asst::InfrastStationTask::run()
{
	if (m_view_ptr == nullptr
		|| m_identify_ptr == nullptr
		|| m_control_ptr == nullptr)
	{
		m_callback(AsstMsg::PtrIsNull, json::value(), m_callback_arg);
		return false;
	}

	cv::Mat image = get_format_image();
	// 先识别一下有几个制造站/贸易站
	const static std::vector<std::string> facility_number_key = { "02", "03", "04", "05" };
	std::vector<Rect> facility_number_rect;
	facility_number_rect.emplace_back(Rect());	// 假装给01 push一个，后面循环好写=。=

	for (const std::string& key : facility_number_key) {
		auto&& [algorithm, score, temp_rect] = m_identify_ptr->find_image(image, key);
		if (score >= Configer::TemplThresholdDefault) {
			facility_number_rect.emplace_back(temp_rect);
		}
		else {
			break;
		}
	}
	for (size_t i = 0; i != facility_number_rect.size(); ++i) {
		if (i != 0) {
			// 点到这个基建
			m_control_ptr->click(facility_number_rect[i]);
			sleep(300);
			image = get_format_image();
		}
		// 如果当前界面没有添加干员的按钮，那就不换班
		auto&& [algorithm1, score1, add_rect1] = m_identify_ptr->find_image(image, "AddOperator");
		auto&& [algorithm2, score2, add_rect2] = m_identify_ptr->find_image(image, "AddOperator-Trade");
		if (score1 < Configer::TemplThresholdDefault && score2 < Configer::TemplThresholdDefault) {
			continue;
		}

		// 识别当前正在造什么
		for (const auto& [key, useless_value] : InfrastConfiger::get_instance().m_infrast_combs) {
			auto&& [algorithm, score, useless_rect] = m_identify_ptr->find_image(image, key);
			if (score >= Configer::TemplThresholdDefault) {
				m_facility = key;
				break;
			}
		}
		//点击添加干员的那个按钮
		Rect add_rect = score1 >= score2 ? std::move(add_rect1) : std::move(add_rect2);
		m_control_ptr->click(add_rect);
		sleep(2000);

		// 点击“清空选择”按钮
		m_control_ptr->click(Rect(430, 655, 150, 40));
		sleep(300);

		auto&& [width, height] = m_view_ptr->getAdbDisplaySize();
		m_swipe_begin = Rect(width * 0.9, height * 0.5, 0, 0);
		m_swipe_end = Rect(width * 0.5, height * 0.5, 0, 0);

		auto detect_ret = swipe_and_detect();
		if (!detect_ret) {
			return false;
		}
		auto cur_opers_info = std::move(detect_ret.value());
		std::list<std::string> optimal_comb = calc_optimal_comb(cur_opers_info);
		bool select_ret = swipe_and_select(optimal_comb);
	}

	return true;
}

std::optional<std::unordered_map<std::string, OperInfrastInfo>> asst::InfrastStationTask::swipe_and_detect()
{
	if (!swipe_to_the_left()) {
		return std::nullopt;
	}

	std::unordered_map<std::string, std::string> feature_cond = InfrastConfiger::get_instance().m_oper_name_feat;
	std::unordered_set<std::string> feature_whatever = InfrastConfiger::get_instance().m_oper_name_feat_whatever;

	std::vector<std::string> end_flag_vec = InfrastConfiger::get_instance().m_infrast_end_flag[m_facility];

	std::unordered_map<std::string, OperInfrastInfo> cur_opers_info;
	// 因为有些干员在宿舍休息，或者被其他基建占用了，所以需要重新识别一遍当前可用的干员
	for (int i = 0; i != SwipeMaxTimes; ++i) {
		const cv::Mat& image = get_format_image(true);
		// 异步进行滑动操作
		std::future<bool> swipe_future = std::async(
			std::launch::async, &InfrastStationTask::swipe, this, false);

		auto cur_name_textarea = detect_opers(image, feature_cond, feature_whatever);
		for (const TextArea& textarea : cur_name_textarea) {
			OperInfrastInfo info;
			// 考虑map中没有这个名字的情况：包括一开始识别漏了、抽到了新干员但没更新等，也有可能是本次识别错了
			// TODO，这里可以抛个回调出去，提示这种case
			if (m_all_opers_info.find(textarea.text) == m_all_opers_info.cend()) {
				info.name = textarea.text;
				info.elite = 0;
				info.level = 0;
			}
			else {
				info = m_all_opers_info[textarea.text];
			}
			cur_opers_info.emplace(textarea.text, std::move(info));
		}

		json::value opers_json;
		std::vector<json::value> opers_json_vec;
		for (const auto& [name, info] : cur_opers_info) {
			json::value info_json;
			info_json["name"] = Utf8ToGbk(info.name);
			info_json["elite"] = info.elite;
			//info_json["level"] = info.level;
			opers_json_vec.emplace_back(std::move(info_json));
		}
		opers_json["all"] = json::array(opers_json_vec);
		m_callback(AsstMsg::OpersDetected, opers_json, m_callback_arg);

		bool break_flag = false;
		// 如果找到了end_flag_vec中的名字，说明已经识别到有当前基建技能的最后一个干员了，就不用接着识别了
		auto find_iter = std::find_first_of(
			cur_opers_info.cbegin(), cur_opers_info.cend(),
			end_flag_vec.cbegin(), end_flag_vec.cend(),
			[](const auto& lhs, const auto& rhs) ->bool {
				return lhs.first == rhs;
			});
		if (find_iter != cur_opers_info.cend()) {
			break_flag = true;
		}
		// 阻塞等待异步的滑动结束
		if (!swipe_future.get()) {
			return std::nullopt;
		}
		if (break_flag) {
			break;
		}
	}
	return cur_opers_info;
}

std::list<std::string> asst::InfrastStationTask::calc_optimal_comb(
	const std::unordered_map<std::string, OperInfrastInfo>& cur_opers_info) const
{
	// 配置文件中的干员组合，和抓出来的干员名比对，如果组合中的干员都有，那就用这个组合
	// 注意配置中的干员组合需要是有序的
	// Todo 时间复杂度起飞了，需要优化下
	std::list<std::string> optimal_comb; // OperInfrastInfo是带精英化和等级信息的，基建里识别不到，也用不到，这里只保留干员名
	for (const auto& name_vec : InfrastConfiger::get_instance().m_infrast_combs[m_facility]) {
		int count = 0;
		std::list<std::string> temp_comb;
		for (const OperInfrastInfo& info : name_vec) {
			// 找到了干员名，而且当前精英化等级需要大于等于配置文件中要求的精英化等级
			if (cur_opers_info.find(info.name) != cur_opers_info.cend()
				&& cur_opers_info.at(info.name).elite >= info.elite) {
				++count;
				temp_comb.emplace_back(info.name);
			}
			else {
				break;
			}
		}
		if (count != 0 && count == name_vec.size()) {
			optimal_comb = temp_comb;
			break;
		}
	}
	std::vector<std::string> optimal_comb_gbk;	// 给回调json用的，gbk的
	for (const std::string& name : optimal_comb) {
		optimal_comb_gbk.emplace_back(Utf8ToGbk(name));
	}
	json::value opers_json;
	opers_json["comb"] = json::array(optimal_comb_gbk);
	m_callback(AsstMsg::InfrastComb, opers_json, m_callback_arg);

	return optimal_comb;
}

bool asst::InfrastStationTask::swipe_and_select(std::list<std::string>& name_comb, int swipe_max_times)
{
	//if (!swipe_to_the_left()) {
	//	return false;
	//}

	std::unordered_map<std::string, std::string> feature_cond = InfrastConfiger::get_instance().m_oper_name_feat;
	std::unordered_set<std::string> feature_whatever = InfrastConfiger::get_instance().m_oper_name_feat_whatever;
	// 一边滑动一边点击最优解中的干员
	for (int i = 0; i != swipe_max_times; ++i) {
		const cv::Mat& image = get_format_image(true);
		auto cur_name_textarea = detect_opers(image, feature_cond, feature_whatever);

		for (TextArea& text_area : cur_name_textarea) {
			// 点过了就不会再点了，直接从最优解vector里面删了
			auto iter = std::find(name_comb.begin(), name_comb.end(), text_area.text);
			if (iter != name_comb.end()) {
				m_control_ptr->click(text_area.rect);
				sleep(200);
				name_comb.erase(iter);
			}
		}
		if (name_comb.empty()) {
			break;
		}
		// 因为滑动和点击是矛盾的，这里没法异步做
		if (!swipe(true)) {
			return false;
		}
	}
	// 点击“确定”按钮，确定完要联网加载的，比较慢，多sleep一会
	get_format_image();	// 这里get image没什么用，单纯就是为了触发下设置缩放，TODO 优化下
	m_control_ptr->click(Rect(1105, 655, 150, 40));
	sleep(2000);
	return true;
}

bool asst::InfrastStationTask::swipe_to_the_left()
{
	set_control_scale(1.0);
	m_swipe_duration = 100;
	m_swipe_extra_delay = 0;
	// 往左使劲滑几下
	bool ret = false;
	for (int i = 0; i != 5; ++i) {
		ret = swipe(true);
		if (!ret) {
			break;
		}
	}
	m_swipe_duration = SwipeDurationDefault;
	m_swipe_extra_delay = SwipeExtraDelayDefault;
	sleep(SwipeExtraDelayDefault);
	return ret;
}
