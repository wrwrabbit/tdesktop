const fs = require('node:fs/promises');
const path = require('node:path');
const assets = require('./release_assets.json');

module.exports = async ({ github, context, releaseId, version, platforms }) => {
    if (!/^\d+\.\d+\.\d+$/.test(version)) {
        throw new Error(`Invalid release version: ${version}`);
    }
    const uploads = [];
    for (const asset of assets.filter(asset => platforms.includes(asset.platform))) {
        const name = `${asset.prefix}-${version}.${asset.extension}`;
        const data = await fs.readFile(path.join(asset.directory, name));
        uploads.push({ name, label: asset.label, data });
        uploads.push({ name: `${asset.prefix}.latest.${asset.extension}`, data });
    }
    for (const asset of uploads) {
        await github.rest.repos.uploadReleaseAsset({
            owner: context.repo.owner,
            repo: context.repo.repo,
            release_id: releaseId,
            ...asset,
        });
    }
};
