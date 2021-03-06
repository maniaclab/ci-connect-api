#ifndef SLATE_USER_COMMANDS_H
#define SLATE_USER_COMMANDS_H

#include "crow.h"
#include "Entities.h"
#include "PersistentStore.h"
#include "rapidjson/document.h"
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"

crow::response listUsers(PersistentStore& store, const crow::request& req);
crow::response createUser(PersistentStore& store, const crow::request& req);
crow::response getUserInfo(PersistentStore& store, const crow::request& req, const std::string uID);
crow::response updateUser(PersistentStore& store, const crow::request& req, const std::string uID);
crow::response deleteUser(PersistentStore& store, const crow::request& req, const std::string uID);
crow::response listUserGroups(PersistentStore& store, const crow::request& req, const std::string uID);
crow::response listUserGroupRequests(PersistentStore& store, const crow::request& req, const std::string uID);
crow::response setUserStatusInGroup(PersistentStore& store, const crow::request& req, 
                                    const std::string& uID, std::string groupID);
crow::response removeUserFromGroup(PersistentStore& store, const crow::request& req, 
                                   const std::string& uID, std::string groupID);
crow::response getUserAttribute(PersistentStore& store, const crow::request& req, 
                                std::string uID, std::string attributeName);
crow::response setUserAttribute(PersistentStore& store, const crow::request& req, 
                                std::string uID, std::string attributeName);
crow::response deleteUserAttribute(PersistentStore& store, const crow::request& req, 
                                   std::string uID, std::string attributeName);
crow::response findUser(PersistentStore& store, const crow::request& req);
crow::response checkUnixName(PersistentStore& store, const crow::request& req);
crow::response replaceUserToken(PersistentStore& store, const crow::request& req,
                                const std::string uID);
crow::response updateLastUseTime(PersistentStore& store, const crow::request& req,
                                 const std::string uID);
                                 
bool validateSSHKeys(const std::string& keyData);

///\param userID the user whose admin status should be checked
///\param groupName the name of the group whose parent groups should be checked 
///                 for the given user's status. This group itself is _not_ checked. 
///\return the name of most closely enclosing group of which the user is an admin, 
///        or the empty string if the user is not an admin of any enclosing group
std::string adminInAnyEnclosingGroup(PersistentStore& store, const std::string& userID, std::string groupName);

#endif //SLATE_USER_COMMANDS_H
