'use strict';
/*
 * deploy.js -- where a deployment's files are: its layout (topology.json, survey.json) and its
 * terrain cache. One copy of the tools serves every site; the site is named, not copied.
 *
 * Named by --deployment or IOTDATA_DEPLOYMENT, either as a path, used as given:
 *
 *     /opt/iotdata/src/iotdata-deploy/se-67194      ./se-67194
 *
 * or as a bare name, found under the iotdata-deploy tree beside the iotdata-platform this lives in:
 *
 *     se-67194   ->   <src>/iotdata-deploy/se-67194
 *
 * A deployment keeps its topology files in `topology/`, so that is used when it exists; a directory
 * without one is taken to be that directory itself. Named by neither, it is the current directory, which
 * is how the tools behaved before there was a choice.
 */
const fs = require('fs'),
    path = require('path');

const ROOT = path.resolve(__dirname, '..', '..', '..', '..', 'iotdata-deploy');

function deploymentDir(name) {
    if (!name) return process.cwd();
    const dir = path.isAbsolute(name) || name.startsWith('.') ? path.resolve(name) : path.join(ROOT, name);
    const sub = path.join(dir, 'topology');
    return fs.existsSync(sub) && fs.statSync(sub).isDirectory() ? sub : dir;
}

module.exports = { ROOT, deploymentDir };
