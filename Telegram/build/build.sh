set -e
FullExecPath=$PWD
pushd `dirname $0` > /dev/null
FullScriptPath=`pwd`
popd > /dev/null

arg1="$1"
arg2="$2"
arg3="$3"

CP_MAC_SKIPDMG="${CP_MAC_SKIPDMG:-0}"
CP_ARCH="${CP_ARCH:-}"
CP_FINAL="${CP_FINAL:-}"

if [ ! -d "$FullScriptPath/../../../DesktopPrivate" ]; then
  echo ""
  echo "This script is for building the production version of Telegram Desktop."
  echo ""
  echo "For building custom versions please visit the build instructions page at:"
  echo "https://github.com/telegramdesktop/tdesktop/#build-instructions"
  exit
fi

Error () {
  cd $FullExecPath
  echo "$1"
  exit 1
}

if [ ! -f "$FullScriptPath/target" ]; then
  Error "Build target not found!"
fi

while IFS='' read -r line || [[ -n "$line" ]]; do
  BuildTarget="$line"
done < "$FullScriptPath/target"

while IFS='' read -r line || [[ -n "$line" ]]; do
  set $line
  eval $1="$2"
done < "$FullScriptPath/version"

while IFS='' read -r line || [[ -n "$line" ]]; do
  set $line
  eval $1="$2"
done < "$FullScriptPath/ptg_version"

VersionForPacker="$PtgAppVersion"
if [ "$AlphaVersion" != "0" ]; then
  AppVersion="$AlphaVersion"
  AppVersionStrFull="${AppVersionStr}_${AlphaVersion}"
  AlphaBetaParam="-alpha $AlphaVersion"
  AlphaKeyFile="talpha_${AppVersion}_key"
elif [ "$BetaChannel" == "0" ]; then
  AppVersionStrFull="$AppVersionStr"
  AlphaBetaParam=''
else
  AppVersionStrFull="$AppVersionStr.beta"
  AlphaBetaParam='-beta'
fi

echo ""
HomePath="$FullScriptPath/.."
if [ "$BuildTarget" == "linux" ]; then
  echo "Building version $AppVersionStrFull for Linux 64bit.."
  UpdateFile="tlinuxupd$PtgAppVersion"
  SetupFile="tsetup.$AppVersionStrFull.tar.xz"
  SetupFile="tsetup.latest.tar.xz"
  ProjectPath="$HomePath/../out"
  ReleasePath="$ProjectPath/Release"
  BinaryName="Telegram"
elif [ "$BuildTarget" == "mac" ] ; then
  if [ "$arg1" == "x86_64" ] || [ "$arg1" == "arm64" ]; then
    echo "Building version $AppVersionStrFull for macOS 10.13+ ($arg1).."
    MacArch="$arg1"
    if [ "$arg2" == "request_uuid" ] && [ "$arg3" != "" ]; then
      NotarizeRequestId="$arg3"
    fi
  else
    echo "Building version $AppVersionStrFull for macOS 10.13+.."
    if [ "$arg2" != "" ]; then
      if [ "$arg1" == "request_uuid_x86_64" ]; then
        NotarizeRequestIdAMD64="$arg2"
      elif [ "$arg1" == "request_uuid_arm64" ]; then
        NotarizeRequestIdARM64="$arg2"
      elif [ "$arg1" == "request_uuid" ]; then
        NotarizeRequestId="$arg2"
      fi
    fi
  fi

  # CP_ARCH environment variable: single architecture build
  if [ "$CP_ARCH" != "" ]; then
    echo "CP_ARCH set to $CP_ARCH, building single architecture..."
    # Note: arg1 (MacArch) will be set by caller, or empty for universal
  fi

  #if [ "$AC_USERNAME" == "" ]; then
  #  Error "AC_USERNAME not found!"
  #fi
  UpdateFileAMD64="tmacupd$PtgAppVersion"
  UpdateFileARM64="tarmacupd$PtgAppVersion"
  if [ "$MacArch" == "arm64" ]; then
    UpdateFile="$UpdateFileARM64"
  elif [ "$MacArch" == "x86_64" ]; then
    UpdateFile="$UpdateFileAMD64"
  fi
  ProjectPath="$HomePath/../out"
  ReleasePath="$ProjectPath/Release"
  BinaryName="Telegram"
  if [ "$MacArch" != "" ]; then
    BundleName="$BinaryName.$MacArch.app"
    SetupFile="tsetup.$MacArch.$AppVersionStrFull.dmg"
    SetupFile="tsetup.$MacArch.latest.dmg"
  else
    BundleName="$BinaryName.app"
    SetupFile="tsetup.$AppVersionStrFull.dmg"
    SetupFile="tsetup.latest.dmg"
  fi
elif [ "$BuildTarget" == "macstore" ]; then
  if [ "$AlphaVersion" != "0" ]; then
    Error "Can't build macstore alpha version!"
  fi

  echo "Building version $AppVersionStrFull for Mac App Store.."
  ProjectPath="$HomePath/../out"
  ReleasePath="$ProjectPath/Release"
  BinaryName="Telegram Lite"
  BundleName="$BinaryName.app"
else
  Error "Invalid target!"
fi

if [ "$AlphaVersion" != "0" ]; then
  if [ -d "$ReleasePath/deploy/$AppVersionStrMajor/$AppVersionStrFull" ]; then
    Error "Deploy folder for version $AppVersionStrFull already exists!"
  fi
else
  if [ -d "$ReleasePath/deploy/$AppVersionStrMajor/$AppVersionStr.alpha" ]; then
    Error "Deploy folder for version $AppVersionStr.alpha already exists!"
  fi

  if [ -d "$ReleasePath/deploy/$AppVersionStrMajor/$AppVersionStr.beta" ]; then
    Error "Deploy folder for version $AppVersionStr.beta already exists!"
  fi

  if [ -d "$ReleasePath/deploy/$AppVersionStrMajor/$AppVersionStr" ]; then
    Error "Deploy folder for version $AppVersionStr already exists!"
  fi

  if [ -f "$ReleasePath/$UpdateFile" ]; then
    Error "Update file for version $AppVersion already exists!"
  fi
fi

DeployPath="$ReleasePath/deploy"

if [ "$BuildTarget" == "linux" ]; then

  DropboxSymbolsPath="$ProjectPath/Dropbox/Telegram/symbols"
  if [ ! -d "$DropboxSymbolsPath" ]; then
    Error "Dropbox path not found!"
  fi

  BackupPath="$ProjectPath/backup/tdesktop/$AppVersionStrMajor/$AppVersionStrFull/t$BuildTarget"
  if [ ! -d "$ProjectPath/backup/tdesktop" ]; then
    Error "Backup folder not found!"
  fi

  # Already running in Docker, no nested virtualization
  cd $HomePath
  $FullScriptPath/docker/build.sh
  echo "Build completed - $?"

  echo "Copying from docker result folder."
  cp "$ReleasePath/root/$BinaryName" "$ReleasePath/$BinaryName"
  cp "$ReleasePath/root/Updater" "$ReleasePath/Updater"
  cp "$ReleasePath/root/Packer" "$ReleasePath/Packer"

  echo "Dumping debug symbols.."
  #"$ReleasePath/dump_syms" "$ReleasePath/$BinaryName" > "$ReleasePath/$BinaryName.sym"
  touch "$ReleasePath/$BinaryName.sym"
  echo "Done!"

  echo "Stripping the executable.."
  strip -s "$ReleasePath/$BinaryName"
  echo "Done!"

  echo "Preparing version $AppVersionStrFull, executing Packer.."
  cd "$ReleasePath"
  "./Packer" -path "$BinaryName" -path Updater -version $VersionForPacker $AlphaBetaParam
  echo "Packer done!"

  if [ "$AlphaVersion" != "0" ]; then
    if [ ! -f "$ReleasePath/$AlphaKeyFile" ]; then
      Error "Alpha version key file not found!"
    fi

    while IFS='' read -r line || [[ -n "$line" ]]; do
      AlphaSignature="$line"
    done < "$ReleasePath/$AlphaKeyFile"

    UpdateFile="${UpdateFile}_${AlphaSignature}"
    SetupFile="talpha${AlphaVersion}_${AlphaSignature}.tar.xz"
  fi

  SymbolsHash=`head -n 1 "$ReleasePath/$BinaryName.sym" | awk -F " " 'END {print $4}'`
  echo "Copying $BinaryName.sym to $DropboxSymbolsPath/$BinaryName/$SymbolsHash"
  mkdir -p "$DropboxSymbolsPath/$BinaryName/$SymbolsHash"
  cp "$ReleasePath/$BinaryName.sym" "$DropboxSymbolsPath/$BinaryName/$SymbolsHash/"
  echo "Done!"

  if [ ! -d "$ReleasePath/deploy" ]; then
    mkdir "$ReleasePath/deploy"
  fi

  #if [ ! -d "$ReleasePath/deploy/$AppVersionStrMajor" ]; then
  #  mkdir "$ReleasePath/deploy/$AppVersionStrMajor"
  #fi

  echo "Copying $BinaryName, Updater and $UpdateFile to $DeployPath/$BinaryName..";
  #mkdir "$DeployPath"
  mkdir "$DeployPath/$BinaryName"
  ls
  mv "$ReleasePath/$BinaryName" "$DeployPath/$BinaryName/"
  mv "$ReleasePath/Updater" "$DeployPath/$BinaryName/"
  mv "$ReleasePath/$UpdateFile" "$DeployPath/"
  if [ "$AlphaVersion" != "0" ]; then
    mv "$ReleasePath/$AlphaKeyFile" "$DeployPath/"
  fi
  cd "$DeployPath"
  ls
  tar -cJvf "$SetupFile" "$BinaryName/"

  mkdir -p $BackupPath
  cp "$SetupFile" "$BackupPath/"
  cp "$UpdateFile" "$BackupPath/"
  if [ "$AlphaVersion" != "0" ]; then
    cp -v "$AlphaKeyFile" "$BackupPath/"
  fi
fi

if [ "$BuildTarget" == "mac" ] || [ "$BuildTarget" == "macstore" ]; then

  DropboxSymbolsPath="$ProjectPath/Dropbox/Telegram/symbols"
  if [ ! -d "$DropboxSymbolsPath" ]; then
    Error "Dropbox path not found!"
  fi

  BackupPath="$ProjectPath/backup/tdesktop/$AppVersionStrMajor/$AppVersionStrFull"
  if [ ! -d "$ProjectPath/backup/tdesktop" ]; then
    Error "Backup path not found!"
  fi

  if [ "$MacArch" != "" ]; then
    cd $ReleasePath

    echo "Preparing single $MacArch .app.."
    # If CP_ARCH was used, the binary is already thin (single arch), just use it
    # If not (recursive call from universal build), thin it from the universal binary
    if [ "$CP_ARCH" != "" ]; then
      # Already thin from build, just copy to expected location
      echo "Using pre-built thin binary for $MacArch"
      rm -rf $BundleName
      cp -R $BinaryName.app $BundleName
    else
      # Thin from universal binary (recursive call case)
      rm -rf $BundleName
      cp -R $BinaryName.app $BundleName
      lipo -thin $MacArch $BinaryName.app/Contents/MacOS/$BinaryName -output $BundleName/Contents/MacOS/$BinaryName
      lipo -thin $MacArch $BinaryName.app/Contents/Frameworks/Updater -output $BundleName/Contents/Frameworks/Updater
      lipo -thin $MacArch $BinaryName.app/Contents/Helpers/crashpad_handler -output $BundleName/Contents/Helpers/crashpad_handler
    fi
    echo "Done!"
  elif [ "$NotarizeRequestId" == "" ]; then
    if [ "$NotarizeRequestIdAMD64" == "" ] && [ "$NotarizeRequestIdARM64" == "" ]; then
      if [ -f "$ReleasePath/$BinaryName.app/Contents/Info.plist" ]; then
        rm "$ReleasePath/$BinaryName.app/Contents/Info.plist"
      fi
      if [ -f "$ProjectPath/Telegram/CMakeFiles/Telegram.dir/Info.plist" ]; then
        rm "$ProjectPath/Telegram/CMakeFiles/Telegram.dir/Info.plist"
      fi
      rm -rf "$ReleasePath/$BinaryName.app/Contents/_CodeSignature"
      rm -rf "$ReleasePath/Updater"

      cd $HomePath
      if [ "$CP_ARCH" != "" ]; then
        ./configure.sh -D DESKTOP_APP_MAC_ARCH="$CP_ARCH"
      else
        ./configure.sh -D DESKTOP_APP_MAC_ARCH="arm64;x86_64"
      fi

      cd $ProjectPath
      cmake --build . --config Release --target Telegram
    fi

    if [ ! -d "$ReleasePath/$BinaryName.app" ]; then
      Error "$BinaryName.app not found!"
    fi

    cd $FullExecPath

    if [ "$BuildTarget" == "mac" ]; then
      # Call subphase for each architecture that needs processing
      # If CP_ARCH is not set (empty), process both arm64 and x86_64
      # If CP_ARCH is set, process only that architecture
      if [ "$NotarizeRequestIdAMD64" == "" ]; then
        if [ "$CP_ARCH" == "" ] || [ "$CP_ARCH" == "arm64" ]; then
          echo "Preparing single arm64 update.."
          ./$0 arm64 request_uuid $NotarizeRequestIdARM64
        fi
      fi

      if [ "$CP_ARCH" == "" ] || [ "$CP_ARCH" == "x86_64" ]; then
        echo "Preparing single x86_64 update.."
        ./$0 x86_64 request_uuid $NotarizeRequestIdAMD64
      fi

      echo "Done."
    fi
  fi
  if [ "$NotarizeRequestId" == "" ]; then
    if [ "$BuildTarget" == "mac" ]; then
      if [ ! -f "$ReleasePath/$BundleName/Contents/Frameworks/Updater" ]; then
        Error "Updater not found!"
      fi
      if [ ! -f "$ReleasePath/$BundleName/Contents/Helpers/crashpad_handler" ]; then
        Error "crashpad_handler not found!"
      fi
    fi
    if [ "$BuildTarget" == "macstore" ]; then
      if [ ! -d "$ReleasePath/$BundleName/Contents/Frameworks/Breakpad.framework" ]; then
        Error "Breakpad.framework not found!"
      fi
    fi

    # Do this w/ univeral build, no CP _HACKs only
    if [ "$MacArch-$CP_ARCH" == "-" ]; then
      echo "Dumping debug symbols x86_64 from universal.."
      "$HomePath/../../Libraries/breakpad/src/tools/mac/dump_syms/build/Release/dump_syms" "-a" "x86_64" "$ReleasePath/$BinaryName.app/Contents/MacOS/$BinaryName" > "$ReleasePath/$BinaryName.x86_64.sym" 2>/dev/null
      echo "Done!"

      SymbolsHash=`head -n 1 "$ReleasePath/$BinaryName.x86_64.sym" | awk -F " " 'END {print $4}'`
      echo "Copying $BinaryName.x86_64.sym to $DropboxSymbolsPath/$BinaryName/$SymbolsHash"
      mkdir -p "$DropboxSymbolsPath/$BinaryName/$SymbolsHash"
      cp "$ReleasePath/$BinaryName.x86_64.sym" "$DropboxSymbolsPath/$BinaryName/$SymbolsHash/$BinaryName.sym"
      echo "Done!"

      echo "Dumping debug symbols arm64 from universal.."
      "$HomePath/../../Libraries/breakpad/src/tools/mac/dump_syms/build/Release/dump_syms" "-a" "arm64" "$ReleasePath/$BinaryName.app/Contents/MacOS/$BinaryName" > "$ReleasePath/$BinaryName.arm64.sym" 2>/dev/null
      echo "Done!"

      SymbolsHash=`head -n 1 "$ReleasePath/$BinaryName.arm64.sym" | awk -F " " 'END {print $4}'`
      echo "Copying $BinaryName.arm64.sym to $DropboxSymbolsPath/$BinaryName/$SymbolsHash"
      mkdir -p "$DropboxSymbolsPath/$BinaryName/$SymbolsHash"
      cp "$ReleasePath/$BinaryName.arm64.sym" "$DropboxSymbolsPath/$BinaryName/$SymbolsHash/$BinaryName.sym"
      echo "Done!"
    fi

    echo "Stripping the executable.."
    strip "$ReleasePath/$BundleName/Contents/MacOS/$BinaryName"
    if [ "$BuildTarget" == "mac" ]; then
      strip "$ReleasePath/$BundleName/Contents/Frameworks/Updater"
      strip "$ReleasePath/$BundleName/Contents/Helpers/crashpad_handler"
    fi
    echo "Done!"

    echo "Signing the application.."
    if [ "$PTG_TESTBUILD" == "1" ]; then
      # no certification - we ok for test
      echo "Skip certification"
    elif [ "$BuildTarget" == "mac" ]; then
      # Use PTG Certificate from GitHub Secrets
      if [ ! -f "certificate.p12" ]; then
        echo $MACOS_CERTIFICATE | base64 --decode > certificate.p12
        
        # Delete keychain if it already exists to avoid conflicts
        security delete-keychain build.keychain 2>/dev/null || true
        
        security create-keychain -p ptelegram_pass build.keychain
        security default-keychain -s build.keychain
        security unlock-keychain -p ptelegram_pass build.keychain
        security import certificate.p12 -k build.keychain -P "$MACOS_CERTIFICATE_PWD" -T /usr/bin/codesign
        security set-key-partition-list -S apple-tool:,apple:,codesign: -s -k ptelegram_pass build.keychain
      fi
      if [ "$identity" == "" ]; then
        echo "Find identity"
        identity=$(security find-identity -v | grep Developer | awk -F " " 'END {print $2}')
      fi
      codesign --force --deep -s ${identity} "$ReleasePath/$BundleName" -v --entitlements "$HomePath/Telegram/Telegram.entitlements"
      
      #codesign --force --deep --timestamp --options runtime --sign "Developer ID Application: Telegram FZ-LLC (C67CF9S4VU)" "$ReleasePath/$BundleName" --entitlements "$HomePath/Telegram/Telegram.entitlements"
    elif [ "$BuildTarget" == "macstore" ]; then
      codesign --force --timestamp --options runtime --sign "3rd Party Mac Developer Application: Telegram FZ-LLC (C67CF9S4VU)" "$ReleasePath/$BundleName/Contents/Frameworks/Breakpad.framework/Versions/A/Resources/breakpadUtilities.dylib" --entitlements "$HomePath/Telegram/Breakpad.entitlements"
      codesign --force --deep --timestamp --options runtime --sign "3rd Party Mac Developer Application: Telegram FZ-LLC (C67CF9S4VU)" "$ReleasePath/$BundleName" --entitlements "$HomePath/Telegram/Telegram Lite.entitlements"
      echo "Making an installer.."
      productbuild --sign "3rd Party Mac Developer Installer: Telegram FZ-LLC (C67CF9S4VU)" --component "$ReleasePath/$BundleName" /Applications "$ReleasePath/$BinaryName.pkg"
    fi
    echo "Done!"

    if [ ! -f "$ReleasePath/$BundleName/Contents/Resources/Icon.icns" ]; then
      # TODO: fix, temp until debugged
      #Error "Icon.icns not found in Resources!"
      echo "Icon.icns not found in Resources!"
    fi

    if [ ! -f "$ReleasePath/$BundleName/Contents/MacOS/$BinaryName" ]; then
      # TODO: fix, temp until debugged
      #Error "$BinaryName not found in MacOS!"
      echo "$BinaryName not found in MacOS!"
    fi

    if [ ! -d "$ReleasePath/$BundleName/Contents/_CodeSignature" ]; then
      # TODO: fix, temp until debugged
      #Error "$BinaryName signature not found!"
      echo "$BinaryName signature not found!"
    fi

    if [ "$BuildTarget" == "macstore" ]; then
      if [ ! -f "$ReleasePath/$BinaryName.pkg" ]; then
        # TODO: fix, temp until debugged
        #Error "$BinaryName.pkg not found!"
        echo "$BinaryName.pkg not found!"
      fi
    fi
  fi

  if [ "$BuildTarget" == "mac" ]; then
    cd "$ReleasePath"

    if [ "$CP_ARCH" != "" ]; then
      echo "CP_ARCH set to $CP_ARCH, skipping DMG creation."
    elif [ "$NotarizeRequestId" == "" ]; then
      if [ "$AlphaVersion" == "0" ]; then
        #cp -f tsetup_template.dmg tsetup.temp.dmg
        #TempDiskPath=`hdiutil attach -nobrowse -noautoopenrw -readwrite tsetup.temp.dmg | awk -F "\t" 'END {print $3}'`
        #cp -R "./$BundleName" "$TempDiskPath/"
        #bless --folder "$TempDiskPath/"
        #hdiutil detach "$TempDiskPath"
        #hdiutil convert tsetup.temp.dmg -format UDBZ -ov -o "$SetupFile"
        #rm tsetup.temp.dmg
        # Do simple
        if [ "$CP_MAC_SKIPDMG" != "1" ]; then
          create-dmg \
              --volname "Telegram Desktop" \
              --volicon "./$BundleName/Contents/Resources/AppIcon.icns" \
              --hide-extension "$BundleName" \
              --icon-size 100 \
              --app-drop-link 400 20 \
              --bless \
              --format UDBZ \
              "$SetupFile" \
              "./$BundleName"
        else
          echo "CP_MAC_SKIPDMG=1: Skipping DMG creation."
          # create fake DMG file so the rest of the script works
          if [ ! -f "$ReleasePath/$SetupFile" ]; then
            touch "$ReleasePath/$SetupFile"
          fi
          echo "Fake DMG file created: $ReleasePath/$SetupFile"
        fi
        # do zip as well
        bless --folder "./$BundleName/" --openfolder "$BundleName/"
        zip -r "$BundleName.zip" "$BundleName"
      fi
    fi

    if [ "$AlphaVersion" != "0" ]; then
      cd $ReleasePath
      "./Packer" -path "$BundleName" -target "$BuildTarget" -version $VersionForPacker $AlphaBetaParam -alphakey

      if [ ! -f "$AlphaKeyFile" ]; then
        Error "Alpha version key file not found!"
      fi

      while IFS='' read -r line || [[ -n "$line" ]]; do
        AlphaSignature="$line"
      done < "$ReleasePath/$AlphaKeyFile"

      UpdateFile="${UpdateFile}_${AlphaSignature}"
      UpdateFileAMD64="${UpdateFileAMD64}_${AlphaSignature}"
      UpdateFileARM64="${UpdateFileARM64}_${AlphaSignature}"
      if [ "$MacArch" != "" ]; then
        SetupFile="talpha${AlphaVersion}_${MacArch}_${AlphaSignature}.zip"
      else
        SetupFile="talpha${AlphaVersion}_${AlphaSignature}.zip"
      fi

      if [ "$NotarizeRequestId" == "" ]; then
        rm -rf "$ReleasePath/AlphaTemp"
        mkdir "$ReleasePath/AlphaTemp"
        mkdir "$ReleasePath/AlphaTemp/$BinaryName"
        cp -r "$ReleasePath/$BundleName" "$ReleasePath/AlphaTemp/$BinaryName/"
        cd "$ReleasePath/AlphaTemp"
        zip -r "$SetupFile" "$BinaryName"
        mv "$SetupFile" "$ReleasePath/"
        mv "$BundleName.zip" "$ReleasePath/"
        cd "$ReleasePath"
      fi
    fi
    echo "Skipping notarization process."
    #xcrun notarytool submit "$SetupFile" --keychain-profile "preston" --wait
    #xcrun stapler staple "$ReleasePath/$BundleName"

    if [ "$MacArch" != "" ]; then
      if [ "$CP_ARCH" != "" ]; then
        echo "CP_ARCH set to $CP_ARCH, skipping DMG cleanup."
      else
        rm "$ReleasePath/$SetupFile"
      fi
      echo "Setup file $SetupFile removed."
    elif [ "$AlphaVersion" != "0" ]; then
      rm -rf "$ReleasePath/AlphaTemp"
      mkdir "$ReleasePath/AlphaTemp"
      mkdir "$ReleasePath/AlphaTemp/$BinaryName"
      cp -r "$ReleasePath/$BinaryName.app" "$ReleasePath/AlphaTemp/$BinaryName/"
      cd "$ReleasePath/AlphaTemp"
      zip -r "$SetupFile" "$BinaryName"
      mv "$SetupFile" "$ReleasePath/"
      cd "$ReleasePath"
      echo "Alpha archive re-created."
    #else
      #xcrun stapler staple "$ReleasePath/$SetupFile"
    fi

    if [ "$MacArch" != "" ]; then
      UpdatePackPath="$ReleasePath/update_pack_${MacArch}"
      rm -rf "$UpdatePackPath"
      mkdir "$UpdatePackPath"
      mv "$ReleasePath/$BundleName" "$UpdatePackPath/$BinaryName.app"
      cp "$ReleasePath/Packer" "$UpdatePackPath/"
      cd "$UpdatePackPath"
      "./Packer" -path "$BinaryName.app" -target "$BuildTarget" -version $VersionForPacker -arch $MacArch $AlphaBetaParam
      echo "Packer done!"
      mv "$UpdateFile" "$ReleasePath/"

      if [ "$CP_ARCH" != "" ]; then
        echo "CP_ARCH set to $CP_ARCH, packing bundle as well"

        # do zip as well
        zip -r "$ReleasePath/$BundleName.zip" "$BinaryName.app/"
      fi

      cd "$ReleasePath"
      rm -rf "$UpdatePackPath"
      exit
    fi
  fi

  if [ ! -d "$ReleasePath/deploy" ]; then
    mkdir "$ReleasePath/deploy"
  fi

  #if [ ! -d "$ReleasePath/deploy/$AppVersionStrMajor" ]; then
  #  mkdir "$ReleasePath/deploy/$AppVersionStrMajor"
  #fi

  if [ "$BuildTarget" == "mac" ]; then
    echo "Copying $BinaryName.app, $UpdateFileAMD64 and $UpdateFileARM64 to deploy/$AppVersionStrMajor/$AppVersionStr..";
    #mkdir "$DeployPath"
    mkdir "$DeployPath/$BinaryName"
    cp -r "$ReleasePath/$BinaryName.app" "$DeployPath/$BinaryName/"
    if [ "$CP_ARCH" == "" ]; then
      if [ "$AlphaVersion" != "0" ]; then
        mv "$ReleasePath/$AlphaKeyFile" "$DeployPath/"
      fi
    fi # CP_ARCH not set
    #rm "$ReleasePath/$BinaryName.app/Contents/MacOS/$BinaryName"
    rm "$ReleasePath/$BinaryName.app/Contents/Frameworks/Updater"
    if [ "$CP_ARCH" != "arm64" ]; then
      mv "$ReleasePath/$UpdateFileAMD64" "$DeployPath/"
    fi # no CP_ARCH or CP_ARCH == AMD64
    if [ "$CP_ARCH" != "x86_64" ]; then
      mv "$ReleasePath/$UpdateFileARM64" "$DeployPath/"
    fi # no CP_ARCH or CP_ARCH == ARM64
    if [ "$CP_MAC_SKIPDMG" != "1" ]; then
      if [ "$CP_ARCH" == "" ]; then
        mv "$ReleasePath/$SetupFile" "$DeployPath/"
      fi # no CP_ARCH
    fi
    if [ "$CP_ARCH" == "" ]; then
      mv "$ReleasePath/$BundleName.zip" "$DeployPath/"
    else
      mv "$ReleasePath/$BinaryName.$CP_ARCH.app.zip" "$DeployPath/"
    fi

    if [ "$BuildTarget" == "mac" ]; then
    if [ "$CP_ARCH" == "" ]; then
      mkdir -p "$BackupPath/tmac"
      cp "$DeployPath/$UpdateFileAMD64" "$BackupPath/tmac/"
      cp "$DeployPath/$UpdateFileARM64" "$BackupPath/tmac/"
      cp "$DeployPath/$SetupFile" "$BackupPath/tmac/"
      if [ "$AlphaVersion" != "0" ]; then
        cp -v "$DeployPath/$AlphaKeyFile" "$BackupPath/tmac/"
      fi
    fi # no CP_ARCH
    fi
  elif [ "$BuildTarget" == "macstore" ]; then
    echo "Copying $BinaryName.app to deploy/$AppVersionStrMajor/$AppVersionStr..";
    mkdir "$DeployPath"
    cp -r "$ReleasePath/$BinaryName.app" "$DeployPath/"
    mv "$ReleasePath/$BinaryName.pkg" "$DeployPath/"
    rm "$ReleasePath/$BinaryName.app/Contents/MacOS/$BinaryName"
  fi
fi

echo "Version $AppVersionStrFull is ready!";
#echo -en "\007";
#sleep 1;
#echo -en "\007";
#sleep 1;
#echo -en "\007";
