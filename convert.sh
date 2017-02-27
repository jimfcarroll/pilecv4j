#!/bin/bash

. ./os.sh

usage() {
    echo "USAGE: $0 -s source-dir -d dest-dir -se src-extention -de desti-extention"
    exit 1
}

. ./env.sh

SRCDIR=
DSTDIR=
SE=
DE=

while [ $# -gt 0 ]; do
    case $1 in
        "-s")
            SRCDIR=$2
            shift
            shift
            ;;
        "-d")
            DSTDIR=$2
            shift
            shift
            ;;
        "-se")
            SE=$2
            shift
            shift
            ;;
        "-de")
            DE=$2
            shift
            shift
            ;;
        *)
            usage
            shift
            ;;
    esac
done

if [ "$SRCDIR" = "" -o "$DSTDIR" = "" ]; then
    usage
fi

if [ ! -d "$DSTDIR" ]; then
    echo "\"$DSTDIR\" doesn't appear to be an existing directory."
    usage
fi

if [ ! -d "$SRCDIR" ]; then
    echo "\"$SRCDIR\" doesn't appear to be an existing directory."
    usage
fi

cd "$DSTDIR"
if [ $? -ne 0 ]; then
    echo "ERROR: couldn't cd to \"$DSTDIR\""
fi
DSTDIR=`pwd -P`
cd - >/dev/null

cd "$SRCDIR"
if [ $? -ne 0 ]; then
    echo "ERROR: couldn't cd to \"$SRCDIR\""
fi

FILES=`find . -name "*.$SE"`
TMPIFS="$IFS"
IFS=$(echo -en "\n\b")
for entry in $FILES; do
    BASE=${entry%.*}
    RELDIR=`dirname $entry`
    mkdir -p "$DSTDIR/$RELDIR"
    if [ $? -ne 0 ]; then
        echo "Failed to create directory \"$DSTDIR/$RELDIR\""
        exit 1
    fi
    if [ -f "$DSTDIR/$BASE.$DE" ]; then
        echo "File \"$DSTDIR/$BASE.$DE\" already exists. Skipping."
    else
        echo "converting: \"$entry\""
        CP="`cpath "$MAVEN_REPO/com/jiminger/lib-image/1.0-SNAPSHOT/lib-image-1.0-SNAPSHOT.jar"`$CSEP`cpath "$MAVEN_REPO/com/jiminger/lib-util/1.0-SNAPSHOT/lib-util-1.0-SNAPSHOT.jar"`$CSEP`cpath "$MAVEN_REPO/com/jiminger/opencv-lib-jar/3.1.0/opencv-lib-jar-3.1.0-withlib.jar"`$CSEP`cpath "$MAVEN_REPO/com/jiminger/opencv-lib-jar/3.1.0/opencv-lib-jar-3.1.0.jar"`$CSEP`cpath "$MAVEN_REPO/opencv/opencv/3.1.0/opencv-3.1.0.jar"`$CSEP`cpath "$MAVEN_REPO/commons-io/commons-io/2.0.1/commons-io-2.0.1.jar"`"
        SRC=`cpath "$SRCDIR/$entry"`
        DST=`cpath "$DSTDIR/$BASE.$DE"`
        RESULTS=`java -Xmx5G -cp $CP com.jiminger.image.ImageFile -i "$SRC" -o "$DST"`
        if [ $? -ne 0 ]; then
            echo "FAILED to run the image convert. See the above error."
            echo "Running: "
            echo "--------------------------------------------"
            echo "java -Xmx5G -cp \$CP com.jiminger.image.ImageFile -i \"$SRC\" -o \"$DST\""
            echo "--------------------------------------------"
            echo "$RESULTS"
        fi
        exit 1
    fi
done
IFS="$TMPIFS"
