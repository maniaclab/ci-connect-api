#!/bin/sh

LOCK_FILE="/var/lock/connect_sync"
LOCK_DATA=$(printf '%10u\n' "$$")

acquire_lock(){
	if [ -e "$LOCK_FILE" ]; then
		# check whether the lock is associated with a running process
		OTHER_PID=$(grep -o '[0-9]*$' "$LOCK_FILE")
		if [ "$?" -eq 0 ]; then
			echo "PID from lock file: $OTHER_PID"
			ps -p "$OTHER_PID" > /dev/null
			if [ "$?" -ne 0 ]; then
				echo "Warning: Lock file $LOCK_FILE apparently held by defunct process; proceeding" 1>&2
			else
				echo "Error: Lock file $LOCK_FILE already exists; cowardly refusing to continue" 1>&2
				exit 1
			fi
		else
			echo "Lock file not intelligible"
			echo "Error: Lock file $LOCK_FILE already exists; cowardly refusing to continue" 1>&2
			exit 1
		fi
	fi
	echo "$LOCK_DATA" > "$LOCK_FILE"
	if [ "$?" -ne 0 ]; then
		echo "Error Failed to write to lock file $LOCK_FILE; cowardly refusing to continue" 1>&2
		exit 1
	fi
}

release_lock() {
	if [ ! -e "$LOCK_FILE" ]; then
		echo "Error: Lock file  $LOCK_FILE does not exist" 1>&2
	elif grep -v '^'"${LOCK_DATA}"'$' "$LOCK_FILE" > /dev/null; then
		echo "Error: Lock file  $LOCK_FILE blongs to another process" 1>&2
	else
		rm "$LOCK_FILE"
	fi
}

HELP="Usage: sync_users.sh [OPTION]...

    -b path, --home-base path
        Use path as the base path for home directories
    -e URL, --api-endpoint URL
        Use URL as the endpoint at which to contact the CI-Connect API
    -g group, --group-group group
        Use group as the group membership source group, the group from which to 
        collect subgroups to which users may belong. This can be different from
        the user source group (specified with -u), but should probably be an
        enclosing group of the user source group.
    -h, --help
        Show this help message
    -t token, --api-token token
        Use token when contacting the CI-Connect API
    -u group, --user-group group
        Use group as the user source group, the group from which users are 
        selected to be provisioned
    --wipe
        Remove all users and groups previously provisioned. This operation will
        permanently destroy any data in users' home directories which has not 
        been copied elsewhere. 
    --dry-run
        Report changes which would be made, without actually making any. 
    --clean-home
        When deleting users, delete their home directories as well. 
    --create-gridmap-file
        Create a grid map file for active users
    --create-storage-authzdb-file
        Create a storage authzdb file for active users
    --storage-authzdb-path
        The path to use for users in group for the storage-authzdb file
    --storage-authzdb-local
        The path to the file for any local users to add to the storage-authzb file
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
	elif [ "$arg" = "--wipe" ]; then
		DO_WIPE=1
	elif [ "$arg" = "--clean-home" ]; then
		ERASE_HOME=1
	elif [ "$arg" = "--dry-run" ]; then
		DRY_RUN=1
	elif [ "$arg" = "--create-gridmap-file" ]; then
		if [ "$#" -lt 1 ]; then
			echo "Error: Missing value after $arg option" 1>&2
			exit 1
		fi
		GRID_MAPFILE="$1"
		shift
	elif [ "$arg" = "--create-storage-authzdb-file" ]; then
		if [ "$#" -lt 1 ]; then
			echo "Error: Missing value after $arg option" 1>&2
			exit 1
		fi
		STORAGE_AUTHZDB_FILE="$1"
		shift
	elif [ "$arg" = "--storage-authzdb-path" ]; then
		if [ "$#" -lt 1 ]; then
			echo "Error: Missing value after $arg option" 1>&2
			exit 1
		fi
		STORAGE_AUTHZDB_PATH="$1"
		shift
	elif [ "$arg" = "--storage-authzdb-local" ]; then
		if [ "$#" -lt 1 ]; then
			echo "Error: Missing value after $arg option" 1>&2
			exit 1
		fi
		STORAGE_AUTHZDB_LOCAL="$1"
		shift
	else
		echo "Error: Unexpected argument: $arg" 1>&2
		exit 1
	fi
done

if [ "$DRY_RUN" ]; then
	echo "Dry run; no changes will be made"
fi

USERDEL=userdel
if [ "$ERASE_HOME" ]; then
	USERDEL="userdel -r"
	echo "Home directories of deleted users will be erased"
fi

date -u

if [ "$DO_WIPE" ]; then
	acquire_lock
	echo "Warning: Erasing all provisioned users and groups in 5 seconds"
	sleep 5
	# erase users
	if [ -f existing_users ]; then
		for DEFUNCT_USER in $(cat existing_users); do
			echo "Deleting user $DEFUNCT_USER"
			if [ ! "$DRY_RUN" ]; then
				$USERDEL "$DEFUNCT_USER"
				if [ "$?" -ne 0 ]; then
					echo "Failed to delete user" 1>&2
					exit 1
				fi
				sed '/^'"$DEFUNCT_USER"'$/d' existing_users > existing_users.new
				mv existing_users.new existing_users
			fi
		done
	fi
	# erase groups
	if [ -f existing_groups ]; then
		for DEFUNCT_GROUP in $(cat existing_groups); do
			echo "Deleting group $DEFUNCT_GROUP"
			if [ ! "$DRY_RUN" ]; then
				groupdel "$DEFUNCT_GROUP"
				if [ "$?" -ne 0 ]; then
					echo "Failed to delete group" 1>&2
					exit 1
				fi
				sed '/^'"$DEFUNCT_GROUP"'$/d' existing_groups > existing_groups.new
				mv existing_groups.new existing_groups
			fi
		done
	fi
	release_lock
	exit
fi

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
# If storage-authzdb creation is requested, ensure we have a path
if [ -n "$STORAGE_AUTHZDB_FILE" ] && [ -z "$STORAGE_AUTHZDB_PATH" ]; then
	STORAGE_AUTHZDB_PATH=''
	echo "STORAGE_AUTHZDB_PATH not set and file generation requested, using default value \"$STORAGE_AUTHZDB_PATH\""
fi


# Ensure that necessary commands are available
ensure_available(){
	if ! which "$1" >/dev/null 2>&1; then
		echo "Error: Unable to find required command $1" 1>&2
		exit 1
	fi
}
ensure_available curl
ensure_available jq

if [ ! "$DRY_RUN" ]; then
	# Ensure that the existing user/group lists exist
	if [ ! -f existing_users ]; then
		touch existing_users
	fi
	if [ ! -f existing_groups ]; then
		touch existing_groups
	fi
fi

acquire_lock

# Get all members of the group
REQUEST_START=$(date "+%s.%N")
curl -sf ${API_ENDPOINT}/v1alpha1/groups/${USER_SOURCE_GROUP}/members?token=${API_TOKEN} > group_members.json
if [ "$?" -ne 0 ]; then
	echo "Error: Failed to download data from ${API_ENDPOINT}/v1alpha1/groups/${USER_SOURCE_GROUP}/members" 1>&2
	release_lock
	exit 1
fi
REQUEST_END=$(date "+%s.%N")
echo "$REQUEST_START $REQUEST_END" | awk '{print "Request took",($2-$1),"seconds"}'
cat group_members.json
if [ -s group_members.json ]; then
	if jq -es 'if . == [] then null else .[] | .memberships end' group_members.json > /dev/null ; then
		: # file okay
	else
		echo "Error: Unable to parse group membership data" 1>&2
		release_lock
		exit 1
	fi
else
	echo "Error: Got no group membership data" 1>&2
	release_lock
	exit 1
fi
ACTIVE_USERS=$(jq '.memberships | map(select(.state==("admin","active")) | .user_name)' group_members.json | sed -n 's|.*"\([^"]*\)".*|\1|p' | sort)
DISABLED_USERS=$(jq '.memberships | map(select(.state==("disabled")) | .user_name)' group_members.json | sed -n 's|.*"\([^"]*\)".*|\1|p' | sort)
N_ACTIVE=$(/usr/bin/env echo "$ACTIVE_USERS" | wc -l)
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
	USER_BLOCK=$(/usr/bin/env echo "$ACTIVE_USERS" | head -n $TO_FETCH | tail -n $BLOCK_SIZE)
	REQUEST='{'
	SEP=""
	for uname in $USER_BLOCK; do
		REQUEST="${REQUEST}${SEP}"'"/v1alpha1/users/'"$uname?token=${API_TOKEN}"'":{"method":"GET"}'
		SEP=','
	done
	REQUEST="${REQUEST}"'}'
	/usr/bin/env echo "$REQUEST" > user_request
	curl -sf -X POST --data '@user_request' ${API_ENDPOINT}/v1alpha1/multiplex?token=${API_TOKEN} > raw_user_data
	if [ "$?" -ne 0 ]; then
		echo "Error: Failed to download data from ${API_ENDPOINT}/v1alpha1/multiplex" 1>&2
		release_lock
		exit 1
	fi
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

# Figure out the last component of the base group name, e.g. root.foo -> foo
BASE_GROUP_NAME=$(/usr/bin/env echo "$GROUP_ROOT_GROUP" | sed 's/.*\.\([^.]*\)$/\1/')
# Figure out what, if anything, contains the base group, e.g. root.foo.bar -> root.foo
BASE_GROUP_CONTEXT=$(/usr/bin/env echo "$GROUP_ROOT_GROUP" | sed -n 's/^\(.*\)\.[^.]*$/\1/p')
if [ "$BASE_GROUP_CONTEXT" ]; then
	# demand an explicit dot after a non-empty base
	BASE_GROUP_CONTEXT="$BASE_GROUP_CONTEXT."
fi
# Get all subgroups
curl -sf ${API_ENDPOINT}/v1alpha1/groups/${GROUP_ROOT_GROUP}/subgroups?token=${API_TOKEN} > subgroups.json
if [ "$?" -ne 0 ]; then
	echo "Error: Failed to download data from ${API_ENDPOINT}/v1alpha1/groups/${GROUP_ROOT_GROUP}/subgroups" 1>&2
	release_lock
	exit 1
fi
SUBGROUPS=$(jq '.groups | map(.name)' subgroups.json | sed -n 's|.*"'"$BASE_GROUP_CONTEXT"'\([^"]*\)".*|\1|p')

# Check that the existing users list is correct, at least to the extent that all listed users do exist
for USER in $(cat existing_users); do
	if grep "^${USER}:" /etc/passwd > /dev/null; then
		: # user exists; good
	else
		echo "User $USER is expected to exist, but does not"
		if [ ! "$DRY_RUN" ]; then
			sed '/^'"$USER"'$/d' existing_users > existing_users.new
			mv existing_users.new existing_users
		fi
	fi
done

# Delete all existing users which should not exist
echo "$ACTIVE_USERS
$DISABLED_USERS" | sort > all_users
for DEFUNCT_USER in $(join -v1 existing_users all_users); do
	echo "Deleting user $DEFUNCT_USER"
	if [ ! "$DRY_RUN" ]; then
		$USERDEL "$DEFUNCT_USER"
		if [ "$?" -eq 0 ]; then
			sed '/^'"$DEFUNCT_USER"'$/d' existing_users > existing_users.new
			mv existing_users.new existing_users
		else
			echo "Error: Failed to delete user $DEFUNCT_USER" 1>&2
		fi
	fi
done
rm all_users

# Delete all existing groups which should not exist
# Do this after deleting users in case any of the groups we need to delete was 
# the primary group of a user which was deleted. 
for DEFUNCT_GROUP in $(printf "%s\n%s" "$BASE_GROUP_NAME" "$SUBGROUPS" | sort | join -v1 existing_groups -); do
	echo "Deleting group $DEFUNCT_GROUP"
	if [ ! "$DRY_RUN" ]; then
		groupdel "$DEFUNCT_GROUP"
		sed '/^'"$DEFUNCT_GROUP"'$/d' existing_groups > existing_groups.new
		mv existing_groups.new existing_groups
	fi
done

# Create groups which are needed and don't yet exist
if grep -q "^${BASE_GROUP_NAME}:" /etc/group; then
	echo "Group $BASE_GROUP_NAME already exists"
else
	GID=$(curl -sf "${API_ENDPOINT}/v1alpha1/groups/${GROUP_ROOT_GROUP}?token=${API_TOKEN}" | jq -r '.metadata.unix_id')
	if [ "$?" -ne 0 ]; then
		echo "Error: Failed to download data from ${API_ENDPOINT}/v1alpha1/groups/${GROUP_ROOT_GROUP}" 1>&2
		release_lock
		exit 1
	fi
	echo "Creating group $BASE_GROUP_NAME with gid $GID"
	if [ ! "$DRY_RUN" ]; then
		groupadd "$BASE_GROUP_NAME" -g $GID
		if [ "$?" -ne 0 ]; then
			echo "Aborting due to group creation error" 1>&2
			release_lock
			exit 1
		fi
	fi
fi
for GROUP in $SUBGROUPS; do
	GID=$(jq -r '.groups | map(select(.name==("'"${BASE_GROUP_CONTEXT}${GROUP}"'"))) | map(.unix_id)[0]' subgroups.json)
	if grep -q "^${GROUP}:" /etc/group; then
		echo "Group $GROUP already exists"
		ACTUAL_GROUP_ID=$(sed -n 's|^'"$GROUP"':[^:]*:\([0-9]*\):.*|\1|p' < /etc/group)
		if [ "$ACTUAL_GROUP_ID" != "$GID" ]; then
			echo "Warning: in-use gid for ${GROUP} (${ACTUAL_GROUP_ID}) does not match expected gid (${GID})"
		fi
	else
		echo "Creating group $GROUP with gid $GID"
		if [ ! "$DRY_RUN" ]; then
			groupadd "$GROUP" -g $GID
			if [ "$?" -ne 0 ]; then
				echo "Aborting due to group creation error" 1>&2
				release_lock
				exit 1
			fi
		fi
	fi
done
if [ ! "$DRY_RUN" ]; then
	printf "%s\n%s" "$BASE_GROUP_NAME" "$SUBGROUPS" | cat existing_groups - | sort | uniq > existing_groups.new
	mv existing_groups.new existing_groups
fi

USERS_TO_CREATE=$(echo "$ACTIVE_USERS" | join -v2 existing_users -)
USERS_TO_UPDATE=$(echo "$ACTIVE_USERS" | join existing_users -)


set_osg_disk_quotas(){
	USER="$1"
	mkdir -p /public/"$USER"
	chown "$USER": /public/"$USER"
	# We might want to factor these out and make them configurable.
	# 50G/100G for $HOME, 500G for Ceph
	# first check that we don't have a quota set. if we do, we don't want to bulldoze over it
	CURRENT_CEPH_QUOTA=$(getfattr --only-values -n ceph.quota.max_bytes /public/"$USER" 2>/dev/null)
	if [ $? -ne 0 ]; then
		setfattr -n ceph.quota.max_bytes -v 500000000000 /public/"$USER"
	else 
		echo "$USER already has a quota of $CURRENT_CEPH_QUOTA"
	fi

	CURRENT_XFS_QUOTA=$(xfs_quota -x -c 'report' /home | grep "$USER")
	if [ $? -ne 0 ]; then
		xfs_quota -x -c "limit -u bsoft=50000000000 bhard=100000000000 $USER" /home
	else
		echo "$USER already has a quota of $(echo "$CURRENT_XFS_QUOTA" | awk '{print $3}')"
	fi
}

set_connect_home_zfs_quotas(){
	USER="$1"
	zfs create tank/export/connect/"$USER"
	chown -R "$USER": /tank/export/connect/"$USER"
	CURRENT_ZFS_QUOTA=$(zfs get -Hp -o value userquota@"$USER" tank/export/connect/"$USER" 2>/dev/null)
	if [ $? -ne 0 ]; then
		echo "ZFS dataset creation failed for $USER"
	elif [ "$CURRENT_ZFS_QUOTA" -eq 0 ]; then
		zfs set userquota@"$USER"=100GB tank/export/connect/"$USER"
	else
		echo "$USER already has a quota of $CURRENT_ZFS_QUOTA"
	fi
}

set_af_home_zfs_quotas(){
	USER="$1"
	zfs create export/home/"$USER"
	chown -R "$USER": export/home/"$USER"
	CURRENT_ZFS_QUOTA=$(zfs get -Hp -o value userquota@"$USER" export/home/"$USER" 2>/dev/null)
	if [ $? -ne 0 ]; then
		echo "ZFS dataset creation failed for $USER"
	elif [ "$CURRENT_ZFS_QUOTA" -eq 0 ]; then
		zfs set userquota@"$USER"=100GB export/home/"$USER"
	else
		echo "$USER already has a quota of $CURRENT_ZFS_QUOTA"
	fi
}

set_sptlocal_disk_quotas(){
	USER="$1"
	zfs create tank/sptlocal/user/"$USER"
	mkdir /tank/sptlocal/user/"$USER"/public_html
	chown -R "$USER": /tank/sptlocal/user/"$USER"
	CURRENT_ZFS_QUOTA=$(zfs get -Hp -o value userquota@"$USER" tank/sptlocal/user/"$USER" 2>/dev/null)
	if [ $? -ne 0 ]; then
		echo "ZFS dataset creation failed for $USER"
	elif [ "$CURRENT_ZFS_QUOTA" -eq 0 ]; then
		zfs set userquota@"$USER"=1TB tank/sptlocal/user/"$USER"
	else
		echo "$USER already has a quota of $CURRENT_ZFS_QUOTA"
	fi
}

set_sptgrid_disk(){
	USER="$1"
	USER_ID="$2"
	GROUP_ID="$3"
	chimera mkdir /sptgrid/user/"$USER"
	chimera chown "$USER_ID":"$GROUP_ID" /sptgrid/user/"$USER"
}

set_stash_disk(){
	USER="$1"
	mkdir -p /stash/user/"$USER"
	chown "$USER": /stash/user/"$USER"
}

set_ssh_authorized_keys(){
	USER="$1"
	USER_HOME_DIR="$2"
	USER_KEY_DATA="$3"
	if [ ! -d "$USER_HOME_DIR/.ssh" ]; then
		mkdir "$USER_HOME_DIR/.ssh"
	fi
	echo "$USER_KEY_DATA" > "$USER_HOME_DIR/.ssh/authorized_keys.new"
	chown -R "$USER": "$USER_HOME_DIR/.ssh"
	mv "$USER_HOME_DIR/.ssh/authorized_keys.new" "$USER_HOME_DIR/.ssh/authorized_keys"
	chmod 0600 "$USER_HOME_DIR/.ssh/authorized_keys"
}

set_grid_mapfile(){
	USER="$1"
	USER_X509="$2"
	MAP_FILE="$3"
	echo "$USER_X509 $USER" >> $MAP_FILE
}

set_storage_authzdb_file(){
	USER="$1"
	USER_ID="$2"
	GROUP_ID="$3"
	MAP_FILE="$4"
	STORAGE_PATH="$5"
	echo "authorize $USER read-write $USER_ID $GROUP_ID / /$STORAGE_PATH /" >> $MAP_FILE
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
	chown -R "$USER": "$USER_HOME_DIR/.ciconnect"
}

set_forward_file(){
	USER="$1"
	USER_HOME_DIR="$2"
	USER_EMAIL="$3"
	echo "$USER_EMAIL" > "$USER_HOME_DIR/.forward"
}

# Ensure that all active users have accounts
cat /dev/null > new_users
for USER in $USERS_TO_CREATE; do
	if [ -f user_data -a ! -s user_data ]; then
		echo "user_data is empty!" 1>&2
		release_lock
		exit 1
	fi
	USER_DATA=$(jq 'select(.unix_name==("'${USER}'"))' user_data)
	if [ "$(/usr/bin/env echo "$USER_DATA" | jq '.service_account')" = "true" ]; then
		echo "Skipping user $USER which is a service account"
		continue
	fi
	USER_ID=$(/usr/bin/env echo "$USER_DATA" | jq -r '.unix_id')
	USER_NAME=$(/usr/bin/env echo "$USER_DATA" | jq -r '.name')
	USER_EMAIL=$(/usr/bin/env echo "$USER_DATA" | jq -r '.email')
	RAW_USER_GROUPS=$(/usr/bin/env echo "$USER_DATA" | jq '.group_memberships | map(select(.state==("active","admin")) | .name)' | sed -n 's|.*"'"$BASE_GROUP_CONTEXT"'\([^"]*\)".*|\1|p' | sed -n '/^'"$BASE_GROUP_NAME"'/p')
	if [ "$?" -ne 0 ]; then
		echo "Failed to extract group_memberships for user $USER" 1>&2
		release_lock
		exit 1
	fi
	USER_GROUPS=$(/usr/bin/env echo "$RAW_USER_GROUPS" | tr '\n' ',' | sed 's|,$||')
	if [ ! "$DRY_RUN" ]; then
		if [ "$GROUP_ROOT_GROUP" == "root.osg" ]; then
			echo "Creating user $USER with uid $USER_ID and groups $USER_GROUPS (XFS)"
			useradd -c "$USER_NAME" -u "$USER_ID" -m -b "${HOME_DIR_ROOT}" -N -g "$BASE_GROUP_NAME" -G "$USER_GROUPS" "$USER"
			if [ "$?" -ne 0 ]; then
				echo "Failed to create user $USER" 1>&2
				cat existing_users new_users | sort | uniq > existing_users.new
				mv existing_users.new existing_users
				if [ "$?" -ne 0 ]; then
					echo "Failed to replace existing_users file" 1>&2
					release_lock
					exit 1
				fi
				rm new_users
				release_lock
				exit 1
			fi
			set_ssh_authorized_keys "$USER" "${HOME_DIR_ROOT}/${USER}" "$(/usr/bin/env echo "$USER_DATA" | jq -r '.public_key')"
		elif [ "$(hostname -f)" == "nfs.grid.uchicago.edu" ]; then
			echo "Creating ZFS home for user $USER with uid $USER_ID and groups $USER_GROUPS"
			set_connect_home_zfs_quotas "$USER"
			set_ssh_authorized_keys "$USER" "${HOME_DIR_ROOT}/${USER}" "$(/usr/bin/env echo "$USER_DATA" | jq -r '.public_key')"
		elif [ "$(hostname -f)" == "nfs.af.uchicago.edu" ]; then
			echo "Creating ZFS home for user $USER with uid $USER_ID and groups $USER_GROUPS"
			set_af_home_zfs_quotas "$USER"
			set_ssh_authorized_keys "$USER" "${HOME_DIR_ROOT}/${USER}" "$(/usr/bin/env echo "$USER_DATA" | jq -r '.public_key')"
		else
			echo "Creating user $USER with uid $USER_ID and groups $USER_GROUPS (No Home)"
			useradd -c "$USER_NAME" -u "$USER_ID" -b "${HOME_DIR_ROOT}" -N -g "$BASE_GROUP_NAME" -G "$USER_GROUPS" "$USER"
			if [ "$?" -ne 0 ]; then
				echo "Failed to create user $USER" 1>&2
				cat existing_users new_users | sort | uniq > existing_users.new
				mv existing_users.new existing_users
				if [ "$?" -ne 0 ]; then
					echo "Failed to replace existing_users file" 1>&2
					release_lock
					exit 1
				fi
				rm new_users
				release_lock
				exit 1
			fi
		fi
		# OSG specific: Try to pick out the first group to which the user belongs and set it as the default 'project'
		# However, we must not pick 'osg', or any of the login node groups, so we remove these from the list.
		FILTERED_USER_GROUPS=$(/usr/bin/env echo "$RAW_USER_GROUPS" | sed -e '/^'"$BASE_GROUP_NAME"'$/d' -e '/^'"$BASE_GROUP_NAME"'.login-nodes/d')
		DEFAULT_GROUP=$(/usr/bin/env echo "$FILTERED_USER_GROUPS" | head -n 1)
		set_default_project "$USER" "${HOME_DIR_ROOT}/${USER}" "$DEFAULT_GROUP"
		set_forward_file "$USER" "${HOME_DIR_ROOT}/${USER}" "$USER_EMAIL"
		if [ "$GROUP_ROOT_GROUP" == "root.osg" ]; then
			set_osg_disk_quotas "$USER"
		elif [ "$GROUP_ROOT_GROUP" == "root.cms" -o "$GROUP_ROOT_GROUP" == "root.duke" ]; then
			set_stash_disk "$USER"
		fi
		# SPT specific: Create user directories on sptlocal.grid.uchicago.edu and xenon-dcache-head.grid.uchicago.edu only. Sorry...
		if [ "$GROUP_ROOT_GROUP" == "root.spt" ] && [ "$(hostname -f)" == "sptlocal.grid.uchicago.edu" ]; then
			set_sptlocal_disk_quotas "$USER"
		fi
		if [ "$GROUP_ROOT_GROUP" == "root.spt" ] && [ "$(hostname -f)" == "xenon-dcache-head.grid.uchicago.edu" ]; then
			GROUP_ID=$(grep spt: /etc/group | cut -d: -f3)
			set_sptgrid_disk "$USER" "$USER_ID" "$GROUP_ID"
		fi
		echo "$USER" >> new_users
	fi
done
if [ ! "$DRY_RUN" ]; then
	cat existing_users new_users | sort | uniq > existing_users.new
	mv existing_users.new existing_users
	if [ "$?" -ne 0 ]; then
		echo "Failed to replace existing_users file" 1>&2
		release_lock
		exit 1
	fi
	rm new_users
fi

# Ensure that previously existing users have updated information
for USER in $USERS_TO_UPDATE; do
	USER_DATA=$(jq 'select(.unix_name==("'${USER}'"))' user_data)
	if [ $(/usr/bin/env echo "$USER_DATA" | jq '.service_account') = "true" ]; then
		echo "Skipping $USER which is a service account"
		continue
	fi
	EXPECTED_USER_ID=$(/usr/bin/env echo "$USER_DATA" | jq -r '.unix_id')
	USER_NAME=$(/usr/bin/env echo "$USER_DATA" | jq -r '.name')
	USER_EMAIL=$(/usr/bin/env echo "$USER_DATA" | jq -r '.email')
	RAW_USER_GROUPS=$(/usr/bin/env echo "$USER_DATA" | jq '.group_memberships | map(select(.state==("active","admin")) | .name)' | sed -n 's|.*"'"$BASE_GROUP_CONTEXT"'\([^"]*\)".*|\1|p' | sed -n '/^'"$BASE_GROUP_NAME"'/p')
	USER_GROUPS=$(/usr/bin/env echo "$RAW_USER_GROUPS" | tr '\n' ',' | sed 's|,$||')
	echo "Updating user $USER with groups $USER_GROUPS"
	if [ ! "$DRY_RUN" ]; then
		usermod -G "$USER_GROUPS" "$USER"
		set_ssh_authorized_keys "$USER" "${HOME_DIR_ROOT}/${USER}" "$(/usr/bin/env echo "$USER_DATA" | jq -r '.public_key')"
		# OSG specific: Try to pick out the first group to which the user belongs and set it as the default 'project'
		# However, we must not pick 'osg', or any of the login node groups, so we remove these from the list.
		FILTERED_USER_GROUPS=$(/usr/bin/env echo "$RAW_USER_GROUPS" | sed -e '/^'"$BASE_GROUP_NAME"'$/d' -e '/^'"$BASE_GROUP_NAME"'.login-nodes/d')
		DEFAULT_GROUP=$(/usr/bin/env echo "$FILTERED_USER_GROUPS" | head -n 1)
		set_default_project "$USER" "${HOME_DIR_ROOT}/${USER}" "$DEFAULT_GROUP"
		set_forward_file "$USER" "${HOME_DIR_ROOT}/${USER}" "$USER_EMAIL"
	fi
	ACTUAL_USER_ID=$(id -u "$USER")
	if [ "$ACTUAL_USER_ID" != "$EXPECTED_USER_ID" ]; then
		echo "Warning: in-use uid for ${USER} (${ACTUAL_USER_ID}) does not match expected uid (${EXPECTED_USER_ID})"
	fi
done

# Ensure that all disabled users have their ssh keys removed
for USER in $DISABLED_USERS; do
	echo "Disabling user $USER"
	if [ ! "$DRY_RUN" ]; then
		if [ -f "${HOME_DIR_ROOT}/${USER}/.ssh/authorized_keys" ]; then
			cat /dev/null > "${HOME_DIR_ROOT}/${USER}/.ssh/authorized_keys"
		fi
	fi
done

# Generate grid mapfile for active users if requested
if [ -n "$GRID_MAPFILE" ]; then
	GRID_MAPFILE_TMP="${GRID_MAPFILE}.tmp"
	echo "Creating grid mapfile $GRID_MAPFILE; temp file $GRID_MAPFILE_TMP"
	if [ -f "$GRID_MAPFILE_TMP" ]; then
		rm $GRID_MAPFILE_TMP
	fi
	for USER in $ACTIVE_USERS; do
		USER_DATA=$(jq 'select(.unix_name==("'${USER}'"))' user_data)
		if [ $(/usr/bin/env echo "$USER_DATA" | jq '.service_account') = "true" ]; then
			echo "Skipping $USER which is a service account"
			continue
		fi
		USER_X509=$(/usr/bin/env echo "$USER_DATA" | jq '."X.509_DN"')
		if [ "$USER_X509" == '""' ]; then
			echo "Skipping empty DN for user $USER: '$USER_X509'"
			continue
		fi
		echo "Adding user $USER to grid mapfile with DN $USER_X509"
		set_grid_mapfile "$USER" "$USER_X509" "$GRID_MAPFILE_TMP"
	done
	if [ ! "$DRY_RUN" ]; then
		mv "$GRID_MAPFILE_TMP" "$GRID_MAPFILE"
	fi
fi

# Generate storage-authzdb for active users if requested
if [ -n "$STORAGE_AUTHZDB_FILE" ]; then
	STORAGE_AUTHZDB_FILE_TMP="${STORAGE_AUTHZDB_FILE}.tmp"
	echo "Creating storage-authzdb file $STORAGE_AUTHZDB_FILE; temp file $STORAGE_AUTHZDB_FILE_TMP"
	if [ -f "$STORAGE_AUTHZDB_FILE_TMP" ]; then
		rm "$STORAGE_AUTHZDB_FILE_TMP"
	fi
	echo "version 2.1" > "$STORAGE_AUTHZDB_FILE_TMP"
	if [ -n "$STORAGE_AUTHZDB_LOCAL" ]; then
		cat "$STORAGE_AUTHZDB_LOCAL" >> "$STORAGE_AUTHZDB_FILE_TMP"
	fi
	for USER in $ACTIVE_USERS; do
		USER_DATA=$(jq 'select(.unix_name==("'${USER}'"))' user_data)
		if [ $(/usr/bin/env echo "$USER_DATA" | jq '.service_account') = "true" ]; then
			echo "Skipping $USER which is a service account"
			continue
		fi
		USER_ID=$(/usr/bin/env echo "$USER_DATA" | jq -r '.unix_id')
		GROUP_ID=$(grep "$(groups "$USER" | awk '{print $3}'):" /etc/group | cut -d: -f3)
		if grep "${USER}" "$STORAGE_AUTHZDB_LOCAL" > /dev/null 2>&1; then
			echo "User $USER already exists in $STORAGE_AUTHZDB_LOCAL, skipping"
		else
			echo "Adding user $USER to grid mapfile with UID $USER_ID, GID $GROUP_ID, and path $STORAGE_AUTHZDB_PATH"
			set_storage_authzdb_file "$USER" "$USER_ID" "$GROUP_ID" "$STORAGE_AUTHZDB_FILE_TMP" "$STORAGE_AUTHZDB_PATH"
		fi
	done
	if [ ! "$DRY_RUN" ]; then
		mv "$STORAGE_AUTHZDB_FILE_TMP" "$STORAGE_AUTHZDB_FILE"
	fi
fi

release_lock
