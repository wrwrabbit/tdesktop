import requests
from pprint import pprint

from telethon import TelegramClient, events, sync
from telethon.tl.types import Channel, MessageMediaDocument, DocumentAttributeFilename

from telethon import functions, types
import time
import json
import zipfile
import os
import sys
import shutil

import re

from argparse import ArgumentParser
from argparse import RawTextHelpFormatter

############### CONFIG

import config
# # Original android APP_ID
API_ID = config.API_ID
API_HASH = config.API_HASH

GH_REPO = "cpartisans/tdesktop"
GH_REST_URL = f"https://api.github.com/repos/%s" % (GH_REPO)

TG_UPLOAD_CHANNEL = "tdptgFiles"
TG_UPDATE_CHANNEL = "tdptgFeed"

# https://github.com/settings/tokens
GH_TOKEN = config.GH_TOKEN
GH_HDR = {'Authorization': "Bearer " + GH_TOKEN}

############### UTILS

def parse_args():
    parser = ArgumentParser(formatter_class=RawTextHelpFormatter)
    parser.add_argument("action", choices = ['list', 'upload', 'files', 'publish'], help = """# list : List and download GH built artifacts
# upload : Upload selected artifacts to TG
# files : List uploaded artifacts from TG
# publish : Update release JSON in TG to reference new builds""")

    ## List options
    parser.add_argument("--failed", action = "store_true", help = "Look through failed builds")
    parser.add_argument("--dry-run", action = "store_true", help = "Don't modify TG")
    parser.add_argument("--latest", action = "store_true", help = "Publish all latest builds")
    parser.add_argument("--testing", action = "store_true", default = None, help = "Publish test builds")
    parser.add_argument("--beta", action = "store_true", help = "Publish beta builds")
    parser.add_argument("--limit", type = int, default = 20, help = "Number of runs to look through")
    # parser.add_argument("--platform", help = "Filter by platform")
    parser.add_argument("ids", type=int, action="store", nargs='*', help = "id(s) of artifacts to download/upload (for upload or publish actions)")
    return parser.parse_args()

def tg_login():
    client = TelegramClient('deploy', API_ID, API_HASH)
    return client

def tg_get_json(client):
    channel = client.get_entity(TG_UPDATE_CHANNEL)
    for msg in client.iter_messages(channel, 2):
        if msg.message:
            return json.loads(msg.message)

def tg_set_json(client, newdata):
    channel = client.get_entity(TG_UPDATE_CHANNEL)
    msg = json.dumps(newdata)
    print("Publish updated JSON...")
    client.send_message(channel, msg)

"""
[{
  "name": string      # file name
  "id": int,          # tg msg_id
  "version": string,  # tg version
  "platform": string, # tg json platform name
  "fullid": string    # tg json id with version and channel name
}]
"""
def tg_list_files(client, limit):
    channel = client.get_entity(TG_UPLOAD_CHANNEL)
    files = []
    for msg in client.iter_messages(channel, limit):
        media = msg.media
        if isinstance(media, MessageMediaDocument):
            doc = media.document
            for attr in doc.attributes:
                if isinstance(attr, DocumentAttributeFilename):
                    m = re.match("(\w+?)(\d{7})", attr.file_name)
                    if m:
                        files.append({
                            "name": attr.file_name,
                            "id": msg.id,
                            "version": m.group(2),
                            "platform": PREFIX.get(m.group(1)),
                            "fullid": "%s:%s#%s" % (m.group(2), TG_UPLOAD_CHANNEL, msg.id)
                            })
    return files

def tg_upload(client, fn, fpath):
    channel = client.get_entity(TG_UPLOAD_CHANNEL)
    print("Uploading...")
    file = client.upload_file(fpath)
    file.name = fn
    print("Posting...")
    client.send_file(channel, file)
    print("Done...")

def update_json(data, platform, url, is_beta = None, is_testing = None):
    rec = data.get(platform, {})
    
    def update_rec(rec, is_testing, url):
        if is_testing == None or is_testing == True:
            rec["testing"] = url
        if is_testing == None or is_testing == False:
            rec["released"] = url
    
    if is_beta == None or is_beta == True:
        rec2 = rec.get("beta", {})
        update_rec(rec2, is_testing, url)
        rec["beta"] = rec2
    if is_beta == None or is_beta == False:
        rec2 = rec.get("stable", {})
        update_rec(rec2, is_testing, url)
        rec["stable"] = rec2
        
    data[platform] = rec
    
    return data

def get_available_builds(args):
    if not os.path.exists("stage"):
        os.makedirs("stage")
    
    # url to request
    url = f"%s/actions/runs?per_page=%s" % (GH_REST_URL, args.limit)
    # make the request and return the json
    runs = requests.get(url, headers=GH_HDR).json()
    # pretty print JSON data
    runs = [{
        "conclusion": r["conclusion"],
        "created_at": r["created_at"],
        "head_branch": r["head_branch"],
        "artifacts_url": r["artifacts_url"],
        "path": r["path"],
        "id": r["id"],
        "status": r["status"],
        "workflow_id": r["workflow_id"],
        } for r in runs.get("workflow_runs", [])]
    runs = [r for r in runs if (
        r["status"] == "completed"
        and
        r["head_branch"] == "master"
        and
        (r["conclusion"] == "success" or args.failed)
        and 
        r["path"].startswith(".github/workflows/deploy_")
        )]
    builds = {}
    for r in runs:
        # if r["path"] in builds:
        #     continue
        # get artifacts
        url = r["artifacts_url"]
        artifacts = requests.get(url, headers=GH_HDR).json()
        arts = {}
        for art in artifacts["artifacts"]:
            art_id = art["id"]
            zip_fn = "stage/%s.zip" % (art_id)
            dl_url = "https://github.com/%s/actions/runs/%s/artifacts/%s" % (GH_REPO, r["id"], art_id)
            dl_url = art["archive_download_url"]
            try:
                if not os.path.isfile(zip_fn):
                    print("Downloading artifacts for %s of %s" % (art_id, r["id"]))
                    zip_stream = requests.get(dl_url, headers=GH_HDR)
                    with open(zip_fn, "wb") as f:
                        f.write(zip_stream.content)
                if not os.path.isfile(zip_fn):
                    continue
                with zipfile.ZipFile(zip_fn) as zip:
                    arts[art_id] = zip.namelist()
            except:
                # if os.path.isfile(zip_fn):
                #     os.unlink(zip_fn)
                raise
        if not arts:
            continue

        builds[r["id"]] = r
        builds[r["id"]]["artifacts"] = arts
    return builds

class FileData:
    fn = None
    path = None

def get_file_data(art_id):
    result = []
    stage = os.path.join("stage", "%s.zip" % (art_id))
    stage_tmp = os.path.join("stage", "%s" % (art_id))
    if not os.path.exists(stage):
        print ("Artifact #%s not found" % (art_id))
        return None
    
    with zipfile.ZipFile(stage) as zip:
        fs = zip.namelist()
        fs = [f for f in fs if "." not in f]
        
        if os.path.exists(stage_tmp):
            shutil.rmtree(stage_tmp)
        for fn in fs:
            zip.extract(fn, stage_tmp)
        
            f = FileData()
            f.fn = fn
            f.path = os.path.join(stage_tmp, fn)
        
            result.append(f)

    return result

############### MAIN


# 'win64': {'beta': {'released': '4009010:gg112233112233#8',
#                    'testing': '4009010:gg112233112233#8'},
#           'stable': {'released': '4010002:gg112233112233#8',
#                      'testing': '4010002:gg112233112233#8'}}}
# <platfor>: { beta|stable: { "released|testing": link }}}
# beta-stable - controlled by user 
# testing-released - for QA team, controlled by cheatcodes

##
# Usage
"""
 ./release.py list [--failed] [--limit]
 ./release.py upload [artifact_id ...]
 ./release.py files
 ./release.py publish [--dry-run] [--latest | ids]
"""

PREFIX = {
    "tlinuxupd": "linux", 
    "tx64upd": "win64",
    "tupdate": "win",
    "tarmacupd": "armac",
    "tmacupd": "mac"
}

def do_list(args):
    builds = get_available_builds(args)

    artifacts = []

    for build in builds.values():
        for art_id, files in build["artifacts"].items():
            for fn in files:
                m = re.match("(\w+?)(\d{7})", fn)
                if m:
                    artifacts.append({
                        "ID": art_id,
                        "fn": fn,
                        "version": m.group(2),
                        "platform": PREFIX.get(m.group(1))
                        })

    pprint(artifacts)


def do_upload(args):
    # check id?
    # unpack file!
    newfiles = []
    for fid in args.ids:
        newfiles_ = get_file_data(fid)
        if newfiles_:
            newfiles += newfiles_
    if not newfiles:
        print("No files to upload")
        sys.exit(1)

    # tg upload    
    client = tg_login()
    with client:
        # check duplicates
        existing = tg_list_files(client, args.limit)
        for newfile in newfiles:
            for f in existing:
                if newfile.fn == f["name"]:
                    print("File %s already exists in TG. Skip" % (newfile.fn))
                    pprint(f)
                    continue
        
            print ("Found: %s @%s" % (newfile.fn, newfile.path))
            if args.dry_run:
                print ("Skip uploading")
                return
            tg_upload(client, newfile.fn, newfile.path)
        print ("Done")

def do_files(args):
    
    client = tg_login()
    with client:
        files = tg_list_files(client, args.limit)
        pprint(files)

def do_publish(args):
    # check for to release
    client = tg_login()
    with client:
        files = tg_list_files(client, args.limit)
        pprint(files)
        latest = tg_get_json(client)
        pprint(latest)
        current = json.dumps(latest)
        
        update = []
        if args.latest:
            done = {}
            for f in files:
                if f["platform"] in done:
                    continue
                done[f["platform"]] = 1
                update.append(f)
        else:
            pprint(args.ids)
            for f in files:
                if f["id"] in args.ids:
                    update.append(f)

        for up in update:
            print("Apply %s = %s" % (up["platform"], up["fullid"]))
            update_json(latest, up["platform"], up["fullid"], is_beta = args.beta, is_testing = args.testing)

        new = json.dumps(latest)
        if new == current:
            print("No changes. Skip")
            return
        pprint(latest)
        
        if not args.dry_run:
            tg_set_json(client, latest)
            print ("Done")
        else:
            print("Skip dry-run")

actions = {
    "list": do_list,
    "upload": do_upload,
    "files": do_files,
    "publish": do_publish,
}

def main():
    
    args = parse_args()
    print(args)
    
    if args.action in actions:
        actions[args.action](args)
    else:
        quit(1)
        
    return 0

if __name__ == "__main__":
    exit(main())
