#!/bin/sh

HELP="Usage: sync_users.sh [OPTION]...

    -b path, --home-base path
        Use path as the base path for home directories
    -e URL, --api-endpoint URL
        Use URL as the endpoint at which to contact the CI-Connect API
    -g group, --group-group group
        Use group as the group membership source group, the group from which to 
        collect subgroups to which users  may belong. This can be different from
        the user source group (specified with -u), but should probably be an
        enclosing group of the user source group.
    -h, --help
        Show this help message
    -t token, --api-token token
        Use token when contacting the CI-Connect API
    -u group, --user-group group
        Use group as the user source group, the group from which users are 
        selected to be provisioned
"

# Read command line arguments
# TODO: support --option=value style
while [ "$#" -gt 0 ]
do
	arg="$1"
	shift
	if [ "$arg" = "--help" -o "$arg" = "-h" ]; then
		echo "$HELP"
		exit 0
	elif [ "$arg" = "--api-token" -o "$arg" = "-t" ]; then
		if [ "$#" -lt 1 ]; then
			echo "Error: Missing value after $arg option" 1>&2
			exit 1
		fi
		API_TOKEN="$1"
		shift
	elif [ "$arg" = "--api-endpoint" -o "$arg" = "-e" ]; then
		if [ "$#" -lt 1 ]; then
			echo "Error: Missing value after $arg option" 1>&2
			exit 1
		fi
		API_ENDPOINT="$1"
		shift
	elif [ "$arg" = "--user-group" -o "$arg" = "-u" ]; then
		if [ "$#" -lt 1 ]; then
			echo "Error: Missing value after $arg option" 1>&2
			exit 1
		fi
		USER_SOURCE_GROUP="$1"
		shift
	elif [ "$arg" = "--group-group" -o "$arg" = "-g" ]; then
		if [ "$#" -lt 1 ]; then
			echo "Error: Missing value after $arg option" 1>&2
			exit 1
		fi
		GROUP_ROOT_GROUP="$1"
		shift
	elif [ "$arg" = "--home-base" -o "$arg" = "-b" ]; then
		if [ "$#" -lt 1 ]; then
			echo "Error: Missing value after $arg option" 1>&2
			exit 1
		fi
		HOME_DIR_ROOT="$1"
		shift
	else
		echo "Error: Unexpected argument: $arg" 1>&2
		exit 1
	fi
	
done

# Check that necessary variables are set
if [ -z "$API_TOKEN" ]; then
	echo "Error: API_TOKEN must be set in the environment or with the --api-token option" 1>&2
	exit 1
fi
# This is the Base URL for contacting the connect API
if [ -z "$API_ENDPOINT" ]; then
	API_ENDPOINT='http://www-dev.ci-connect.net:18080'
	echo "API_ENDPOINT not set, using default value $API_ENDPOINT"
fi
# This is the group from which to collect users
if [ -z "$USER_SOURCE_GROUP" ]; then
	echo "Error: USER_SOURCE_GROUP must be set in the environment or with the --user-group option" 1>&2
	exit 1
fi
# This is the group from which to collect subgroups to which users may belong.
# It can be different from USER_SOURCE_GROUP, but should probably be an enclosing group in that case.
if [ -z "$GROUP_ROOT_GROUP" ]; then
	echo "Error: GROUP_ROOT_GROUP must be set in the environment or with the --group-group option" 1>&2
	exit 1
fi
# This is the base path within which users' home directories are provisioned.
if [ -z "$HOME_DIR_ROOT" ]; then
	HOME_DIR_ROOT='/home'
	echo "HOME_DIR_ROOT not set, using default value $HOME_DIR_ROOT"
fi

# Ensure that the existing users list exists
if [ ! -f existing_users ]; then
	touch existing_users
fi
if [ ! -f existing_groups ]; then
	touch existing_groups
fi

# Get all members of the group
curl -s ${API_ENDPOINT}/v1alpha1/groups/${USER_SOURCE_GROUP}/members?token=${API_TOKEN} > group_members.json
ACTIVE_USERS=$(jq '.memberships | map(select(.state==("admin","active")) | .user_name)' group_members.json | sed -n 's|.*"\([^"]*\)".*|\1|p' | sort)
DISABLED_USERS=$(jq '.memberships | map(select(.state==("disabled")) | .user_name)' group_members.json | sed -n 's|.*"\([^"]*\)".*|\1|p' | sort)
N_ACTIVE=`echo "$ACTIVE_USERS" | wc -l`
echo "$N_ACTIVE active group members"
rm group_members.json

# Fetch details about active group members
# Do this in blocks to avoid any single request being too large and potentially timing out
BLOCK_SIZE=1000
TO_FETCH=$N_ACTIVE
PROCESSED=0
cat /dev/null > user_data
while [ "$PROCESSED" -lt "$N_ACTIVE" ]; do
	TO_FETCH=$(expr $PROCESSED + $BLOCK_SIZE)
	if [ "$TO_FETCH" -gt "$N_ACTIVE" ]; then
		TO_FETCH=$N_ACTIVE
		BLOCK_SIZE=$(expr $N_ACTIVE - $PROCESSED)
	fi
	USER_BLOCK=$(echo "$ACTIVE_USERS" | head -n $TO_FETCH | tail -n $BLOCK_SIZE)
	REQUEST='{'
	SEP=""
	for uname in $USER_BLOCK; do
		REQUEST="${REQUEST}${SEP}"'"/v1alpha1/users/'"$uname?token=${API_TOKEN}"'":{"method":"GET"}'
		SEP=','
	done
	REQUEST="${REQUEST}"'}'
	echo "$REQUEST" > user_request
	curl -s -X POST --data '@user_request' ${API_ENDPOINT}/v1alpha1/multiplex?token=${API_TOKEN} > raw_user_data
	jq '.[] | .body | fromjson | .metadata' raw_user_data | sed -e '/"institution"/d' \
		-e '/"access_token"/d' \
		-e '/"phone"/d' \
		-e '/"join_date"/d' \
		-e '/"last_use_time"/d' \
		-e '/"superuser"/d' \
		-e '/"state_set_by"/d' \
		-e 's/\("state": "[^"]*"\),/\1/' >> user_data
	rm user_request raw_user_data
	PROCESSED=$(expr $PROCESSED + $BLOCK_SIZE)
	echo "Fetched $PROCESSED users"
done

# Get all subgroups
curl -s ${API_ENDPOINT}/v1alpha1/groups/${GROUP_ROOT_GROUP}/subgroups?token=${API_TOKEN} > subgroups.json
SUBGROUPS=$(jq '.groups | map(.name)' subgroups.json | sed -n 's|.*"'"$GROUP_ROOT_GROUP"'\.\([^"]*\)".*|\1|p')

# Delete all existing users which should not exist
echo "$ACTIVE_USERS
$DISABLED_USERS" | sort > all_users
for DEFUNCT_USER in $(join -v1 existing_users all_users); do
	echo "Deleting user $DEFUNCT_USER"
	userdel -r "$DEFUNCT_USER"
	sed '/^'"$DEFUNCT_USER"'$/d' existing_users > existing_users.new
	mv existing_users.new existing_users
done
rm all_users

# Delete all existing groups which should not exist
# Do this after deleting users in case any of the groups we need to delete was 
# the primary group of a user which was deleted. 
for DEFUNCT_GROUP in $(echo "$SUBGROUPS" | join -v1 existing_groups -); do
	echo "Deleting group $DEFUNCT_GROUP"
	groupdel "$DEFUNCT_GROUP"
	sed '/^'"$DEFUNCT_GROUP"'$/d' existing_groups > existing_groups.new
	mv existing_groups.new existing_groups
done

# Create groups which are needed and don't yet exist
for GROUP in $SUBGROUPS; do
	if grep -q "^${GROUP}:" /etc/group; then
		echo "Group $GROUP already exists"
	else
		echo "Creating group $GROUP"
		GID=$(jq -r '.groups | map(select(.name==("'"${GROUP_ROOT_GROUP}.${GROUP}"'"))) | map(.unix_id)[0]' subgroups.json)
		echo "group data:"; jq -r '.groups | map(select(.name==("'"${GROUP_ROOT_GROUP}.${GROUP}"'")))' subgroups.json
		groupadd "$GROUP" -g $GID
	fi
done

USERS_TO_CREATE=$(echo "$ACTIVE_USERS" | join -v2 existing_users -)
USERS_TO_UPDATE=$(echo "$ACTIVE_USERS" | join existing_users -)

set_ssh_authorized_keys(){
	USER="$1"
	USER_HOME_DIR="$2"
	USER_KEY_DATA="$3"
	if [ ! -d "$USER_HOME_DIR/.ssh" ]; then
		mkdir "$USER_HOME_DIR/.ssh"
	fi
	echo "$USER_KEY_DATA" > "$USER_HOME_DIR/.ssh/authorized_keys"
	chown -R "$USER" "$USER_HOME_DIR/.ssh"
	chmod 0600 "$USER_HOME_DIR/.ssh/authorized_keys"
}

set_default_project(){
	USER="$1"
	USER_HOME_DIR="$2"
	USER_PROJECT="$3"
	
	# Don't overwrite if the user already has a project file
	if [ -e "$USER_HOME_DIR/.ciconnect/defaultproject" ]; then
		return
	fi
	if [ ! -d "$USER_HOME_DIR/.ciconnect" ]; then
		mkdir "$USER_HOME_DIR/.ciconnect"
	fi
	echo "$USER_PROJECT" > "$USER_HOME_DIR/.ciconnect/defaultproject"
	chown -R "$USER" "$USER_HOME_DIR/.ciconnect"
}

# Ensure that all active users have accounts
cat /dev/null > new_users
for USER in $USERS_TO_CREATE; do
	USER_DATA=$(jq 'select(.unix_name==("'${USER}'"))' user_data)
	if [ $(echo "$USER_DATA" | jq '.service_account') = "true" ]; then
		echo "Skipping user $USER which is a service account"
		continue
	fi
	echo "Creating user $USER"
	USER_ID=$(echo "$USER_DATA" | jq -r '.unix_id')
	USER_NAME=$(echo "$USER_DATA" | jq -r '.name')
	USER_EMAIL=$(echo "$USER_DATA" | jq -r '.email')
	USER_GROUPS=$(echo "$USER_DATA" | jq '.group_memberships | map(select(.state==("active","admin")) | .name)' | sed -n 's|.*"'"$GROUP_ROOT_GROUP"'\.\([^"]*\)".*|\1|p' | tr '\n' ',' | sed 's|,$||')
	useradd -c "$USER_NAME" -u "$USER_ID" -m -b "${HOME_DIR_ROOT}/" -G "$USER_GROUPS" "$USER"
	set_ssh_authorized_keys "$USER" "${HOME_DIR_ROOT}/${USER}" "$(echo "$USER_DATA" | jq -r '.public_key')"
	DEFAULT_GROUP=$(echo "$USER_GROUPS" | sed 's|,.*$||')
	set_default_project "$USER" "${HOME_DIR_ROOT}/${USER}" "$DEFAULT_GROUP"
	echo "$USER" >> new_users
done
cat existing_users new_users | sort | uniq > existing_users.new
mv existing_users.new existing_users
rm new_users

# Ensure that previously existing users have updated information
for USER in $USERS_TO_UPDATE; do
	USER_DATA=$(jq 'select(.unix_name==("'${USER}'"))' user_data)
	if [ $(echo "$USER_DATA" | jq '.service_account') = "true" ]; then
		echo "Skipping $USER which is a service account"
		continue
	fi
	echo "Updating user $USER"
	USER_NAME=$(echo "$USER_DATA" | jq -r '.name')
	USER_EMAIL=$(echo "$USER_DATA" | jq -r '.email')
	USER_GROUPS=$(echo "$USER_DATA" | jq '.group_memberships | map(select(.state==("active","admin")) | .name)' | sed -n 's|.*"'"$GROUP_ROOT_GROUP"'\.\([^"]*\)".*|\1|p' | tr '\n' ',' | sed 's|,$||')
	usermod -G "$USER_GROUPS" "$USER"
	set_ssh_authorized_keys "$USER" "${HOME_DIR_ROOT}/${USER}" "$(echo "$USER_DATA" | jq -r '.public_key')"
	DEFAULT_GROUP=$(echo "$USER_GROUPS" | sed 's|,.*$||')
	set_default_project "$USER" "${HOME_DIR_ROOT}/${USER}" "$DEFAULT_GROUP"
done

# Ensure that all disabled users have their ssh keys removed
for USER in $DISABLED_USERS; do
	echo "Disabling user $USER"
	if [ -f "${HOME_DIR_ROOT}/${USER}/.ssh/authorized_keys" ]; then
		cat /dev/null > "${HOME_DIR_ROOT}/${USER}/.ssh/authorized_keys"
	fi
done
