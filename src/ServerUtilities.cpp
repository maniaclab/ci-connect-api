#include "ServerUtilities.h"

#include <boost/date_time/posix_time/posix_time.hpp>

#include "Logging.h"

std::string timestamp(){
	auto now = boost::posix_time::microsec_clock::universal_time();
	return to_simple_string(now)+" UTC";
}

std::string generateError(const std::string& message){
	rapidjson::Document err(rapidjson::kObjectType);
	err.AddMember("kind", "Error", err.GetAllocator());
	err.AddMember("message", rapidjson::StringRef(message.c_str()), err.GetAllocator());
	
	rapidjson::StringBuffer errBuffer;
	rapidjson::Writer<rapidjson::StringBuffer> writer(errBuffer);
	err.Accept(writer);
  
	return errBuffer.GetString();
}

std::string unescape(const std::string& message){
	std::string result = message;
	std::vector<std::pair<std::string,std::string>> escaped;
	escaped.push_back(std::make_pair("\\n", "\n"));
	escaped.push_back(std::make_pair("\\t", "\t"));
	escaped.push_back(std::make_pair("\\\\", "\\"));
	escaped.push_back(std::make_pair("\\\"", "\""));

	for (auto item : escaped){
		auto replace = item.first;
		auto found = result.find(replace);
		while (found != std::string::npos){
			result.replace(found, replace.length(), item.second);
			found = result.find(replace);
		}
	}
	
	return result;
}

std::string shellEscapeSingleQuotes(const std::string& raw){
	if(raw.empty())
		return raw;
	std::ostringstream ss;
	std::size_t last=0, next;
	while(true){
		next=raw.find('\'',last); //copy data up to quote, which might be all of it
		if(next!=std::string::npos){ //if there is a single quote
			ss << raw.substr(last,next-last);
			if(next) //if not at the start
				ss << '\''; //stop single quoting
			ss << R"(\')"; //insert the escaped quote
			if(next<raw.size()-1) //if more data follows
				ss << '\''; //restart single quoting
			last=next+1; //update portion of string handled so far
		}
		else{
			ss << raw.substr(last); //copy remainder
			break;
		}
	}
	return ss.str();
}

std::string trim(const std::string &s){
    auto wsfront = std::find_if_not(s.begin(),s.end(),[](int c){return std::isspace(c);});
    auto wsback = std::find_if_not(s.rbegin(),s.rend(),[](int c){return std::isspace(c);}).base();
    return (wsback <= wsfront ? std::string() : std::string(wsfront, wsback));
}

std::vector<std::string> string_split_lines(const std::string& text) {
    std::stringstream ss(text);
    std::vector<std::string> lines;
    std::string line;
    while(std::getline(ss, line)){
        lines.push_back(line);
    }
    return lines;
}

std::vector<std::string> string_split_columns(const std::string& line, char delim, bool keepEmpty) {
    std::stringstream ss(line);
    std::vector<std::string> tokens;
    std::string item;
    while (std::getline(ss, item, delim)) {
		auto token=trim(item);
		if(!token.empty() || keepEmpty)
			tokens.push_back(token);
    }
    return tokens;
}
