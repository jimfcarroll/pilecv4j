
DEFAULT_GIT_REPO_HOST=gitlab.kognition.ai
[ "$GIT_REPO_HOST" = "" ] && GIT_REPO_HOST=$DEFAULT_GIT_REPO_HOST

DEFAULT_NEXUS_REPO_HOST=nexus.kognition.ai
[ "$NEXUS_REPO_HOST" = "" ] && NEXUS_REPO_HOST=$DEFAULT_NEXUS_REPO_HOST

NEXUS_REPO_URL=https://$NEXUS_REPO_HOST

