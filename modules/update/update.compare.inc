<?php
// $Id: update.compare.inc,v 1.1 2007/07/11 15:15:40 dries Exp $

/**
 * @file
 * Code required only when comparing available updates to existing data.
 */

/**
 * Fetch an array of installed and enabled projects.
 *
 * This is only responsible for generating an array of projects (taking into
 * account projects that include more than one module or theme). Other
 * information like the specific version and install type (official release,
 * dev snapshot, etc) is handled later in update_process_project_info() since
 * that logic is only required when preparing the status report, not for
 * fetching the available release data.
 *
 * @see update_process_project_info()
 * @see update_calculate_project_data()
 *
 */
function update_get_projects() {
  static $projects = array();
  if (empty($projects)) {
    _update_process_info_list($projects, module_rebuild_cache(), 'module');
    _update_process_info_list($projects, system_theme_data(), 'theme');
  }
  return $projects;
}

/**
 * Populate an array of project data.
 */
function _update_process_info_list(&$projects, &$list, $project_type) {
  foreach ($list as $file) {
    if (empty($file->status)) {
      // Skip disabled modules or themes.
      continue;
    }

    // Skip if the .info file is broken.
    if (empty($file->info)) {
      continue;
    }

    // If the .info doesn't define the 'project', try to figure it out.
    if (!isset($file->info['project'])) {
      $file->info['project'] = update_get_project_name($file);
    }

    if (!isset($projects[$file->info['project']])) {
      // Only process this if we haven't done this project, since a single
      // project can have multiple modules or themes.
      $projects[$file->info['project']] = array(
        'name' => $file->info['project'],
        'info' => $file->info,
        'datestamp' => isset($file->info['datestamp']) ? $file->info['datestamp'] : 0,
        'includes' => array($file->name => $file->info['name']),
        'project_type' => $file->info['project'] == 'drupal' ? 'core' : $project_type,
      );
    }
    else {
      $projects[$file->info['project']]['includes'][$file->name] = $file->info['name'];
    }
  }
}

/**
 * Given a $file object (as returned by system_get_files_database()), figure
 * out what project it belongs to.
 *
 * @see system_get_files_database()
 */
function update_get_project_name($file) {
  $project_name = '';
  if (isset($file->info['project'])) {
    $project_name = $file->info['project'];
  }
  elseif (isset($file->info['package']) && (strpos($file->info['package'], 'Core -') !== FALSE)) {
    $project_name = 'drupal';
  }
  elseif (in_array($file->name, array('bluemarine', 'chameleon', 'garland', 'marvin', 'minnelli', 'pushbutton'))) {
    // Unfortunately, there's no way to tell if a theme is part of core,
    // so we must hard-code a list here.
    $project_name = 'drupal';
  }
  else {
    // This isn't part of core, so guess the project from the directory.
    $last = '';
    foreach (array_reverse(explode('/', $file->filename)) as $dir) {
      if ($dir == 'modules' || $dir == 'themes') {
        break;
      }
      $last = $dir;
    }
    if ($last) {
      $project_name = $last;
    }
  }
  return $project_name;
}

/**
 * Process the list of projects on the system to figure out the currently
 * installed versions, and other information that is required before we can
 * compare against the available releases to produce the status report.
 *
 * @param $projects
 *   Array of project information from update_get_projects().
 */
function update_process_project_info(&$projects) {
  foreach ($projects as $key => $project) {
    // Assume an official release until we see otherwise.
    $install_type = 'official';

    $info = $project['info'];

    if (isset($info['version'])) {
      // Check for development snapshots
      if (preg_match('@(dev|HEAD)@', $info['version'])) {
        $install_type = 'dev';
      }

      // Figure out what the currently installed major version is. We need
      // to handle both contribution (e.g. "5.x-1.3", major = 1) and core
      // (e.g. "5.1", major = 5) version strings.
      $matches = array();
      if (preg_match('/^(\d+\.x-)?(\d+)\..*$/', $info['version'], $matches)) {
        $info['major'] = $matches[2];
      }
      elseif (!isset($info['major'])) {
        // This would only happen for version strings that don't follow the
        // drupal.org convention. We let contribs define "major" in their
        // .info in this case, and only if that's missing would we hit this.
        $info['major'] = -1;
      }
    }
    else {
      // No version info available at all.
      $install_type = 'unknown';
      $info['version'] = t('Unknown');
      $info['major'] = -1;
    }

    // Finally, save the results we care about into the $projects array.
    $projects[$key]['existing_version'] = $info['version'];
    $projects[$key]['existing_major'] = $info['major'];
    $projects[$key]['install_type'] = $install_type;
    unset($projects[$key]['info']);
  }
}

/**
 * Given the installed projects and the available release data retrieved from
 * remote servers, calculate the current status.
 *
 * This function is the heart of the update status feature. It iterates over
 * every currently installed project, and for each one, decides what major
 * release series to consider (the larger of the major version currently
 * installed and the default major version specified by the maintainer of that
 * project).
 *
 * Given a target major version, it scans the available releases looking for
 * the specific release to recommend (avoiding beta releases and development
 * snapshots if possible). This is complicated to describe, but an example
 * will help clarify. For the target major version, find the highest patch
 * level. If there is a release at that patch level with no extra ("beta",
 * etc), then we recommend the release at that patch level with the most
 * recent release date. If every release at that patch level has extra (only
 * betas), then recommend the latest release from the previous patch
 * level. For example:
 *
 * 1.6-bugfix <-- recommended version because 1.6 already exists.
 * 1.6
 *
 * or
 *
 * 1.6-beta
 * 1.5 <-- recommended version because no 1.6 exists.
 * 1.4
 *
 * It also looks for the latest release from the same major version, even a
 * beta release, to display to the user as the "Latest version" option.
 * Additionally, it finds the latest official release from any higher major
 * versions that have been released to provide a set of "Also available"
 * options.
 *
 * Finally, and most importantly, it keeps scanning the release history until
 * it gets to the currently installed release, searching for anything marked
 * as a security update. If any security updates have been found between the
 * recommended release and the installed version, all of the releases that
 * included a security fix are recorded so that the site administrator can be
 * warned their site is insecure, and links pointing to the release notes for
 * each security update can be included (which, in turn, will link to the
 * official security announcements for each vulnerability).
 *
 * This function relies on the fact that the .xml release history data comes
 * sorted based on major version and patch level, then finally by release date
 * if there are multiple releases such as betas from the same major.patch
 * version (e.g. 5.x-1.5-beta1, 5.x-1.5-beta2, and 5.x-1.5). Development
 * snapshots for a given major version are always listed last.
 *
 * @param $available
 *  Array of data about available project releases.
 *
 * @see update_get_available()
 * @see update_get_projects()
 * @see update_process_project_info()
 */
function update_calculate_project_data($available) {
  $projects = update_get_projects();
  update_process_project_info($projects);
  foreach ($projects as $project => $project_info) {
    if (isset($available[$project])) {
      // Figure out the target major version.
      $existing_major = $project_info['existing_major'];
      if (isset($available[$project]['default_major'])) {
        $default_major = $available[$project]['default_major'];
        $target_major = max($existing_major, $default_major);
      }
      else {
        $target_major = $existing_major;
      }

      $version_patch_changed = '';
      $patch = '';

      foreach ($available[$project]['releases'] as $version => $release) {
        // Ignore unpublished releases.
        if ($release['status'] != 'published') {
          continue;
        }

        // See if this is a higher major version than our target, and if so,
        // record it as an "Also available" release.
        if ($release['version_major'] > $target_major) {
          if (!isset($available[$project]['also'])) {
            $available[$project]['also'] = array();
          }
          if (!isset($available[$project]['also'][$release['version_major']])) {
            $available[$project]['also'][$release['version_major']] = $version;
          }
          // Otherwise, this release can't matter to us, since it's neither
          // from the release series we're currently using nor the recommended
          // release. We don't even care about security updates for this
          // branch, since if a project maintainer puts out a security release
          // at a higher major version and not at the lower major version,
          // they must change the default major release at the same time, in
          // which case we won't hit this code.
          continue;
        }

        // Look for the 'latest version' if we haven't found it yet. Latest is
        // defined as the most recent version for the target major version.
        if (!isset($available[$project]['latest_version'])
            && $release['version_major'] == $target_major) {
          $available[$project]['latest_version'] = $version;
        }

        // Look for the development snapshot release for this branch.
        if (!isset($available[$project]['dev_version'])
            && $release['version_major'] == $target_major
            && isset($release['version_extra'])
            && $release['version_extra'] == 'dev') {
          $available[$project]['dev_version'] = $version;
        }

        // Look for the 'recommended' version if we haven't found it yet (see
        // phpdoc at the top of this function for the definition).
        if (!isset($available[$project]['recommended'])
            && $release['version_major'] == $target_major
            && isset($release['version_patch'])) {
          if ($patch != $release['version_patch']) {
            $patch = $release['version_patch'];
            $version_patch_changed = $release['version'];
          }
          if (empty($release['version_extra']) && $patch == $release['version_patch']) {
            $available[$project]['recommended'] = $version_patch_changed;
          }
        }

        // Stop searching once we hit the currently installed version.
        if ($projects[$project]['existing_version'] == $version) {
          break;
        }

        // If we're running a dev snapshot and have a timestamp, stop
        // searching for security updates once we hit an official release
        // older than what we've got.  Allow 100 seconds of leeway to handle
        // differences between the datestamp in the .info file and the
        // timestamp of the tarball itself (which are usually off by 1 or 2
        // seconds) so that we don't flag that as a new release.
        if ($projects[$project]['install_type'] == 'dev') {
          if (empty($projects[$project]['datestamp'])) {
            // We don't have current timestamp info, so we can't know.
            continue;
          }
          elseif (isset($release['date']) && ($projects[$project]['datestamp'] + 100 > $release['date'])) {
            // We're newer than this, so we can skip it.
            continue;
          }
        }

        // See if this release is a security update.
        if (isset($release['terms'])
            && isset($release['terms']['Release type'])
            && in_array('Security update', $release['terms']['Release type'])) {
          $projects[$project]['security updates'][] = $release;
        }
      }

      // If we were unable to find a recommended version, then make the latest
      // version the recommended version if possible.
      if (!isset($available[$project]['recommended']) && isset($available[$project]['latest_version'])) {
        $available[$project]['recommended'] = $available[$project]['latest_version'];
      }

      // If we're running a dev snapshot, compare the date of the dev snapshot
      // with the latest official version, and record the absolute latest in
      // 'latest_dev' so we can correctly decide if there's a newer release
      // than our current snapshot.
      if ($projects[$project]['install_type'] == 'dev') {
        if (isset($available[$project]['dev_version']) && $available[$project]['releases'][$available[$project]['dev_version']]['date'] > $available[$project]['releases'][$available[$project]['latest_version']]['date']) {
          $projects[$project]['latest_dev'] = $available[$project]['dev_version'];
        }
        else {
          $projects[$project]['latest_dev'] = $available[$project]['latest_version'];
        }
      }

      // Stash the info about available releases into our $projects array.
      $projects[$project] += $available[$project];

      //
      // Check to see if we need an update or not.
      //

      // If we don't know what to recommend, there's nothing much we can
      // report, so bail out early.
      if (!isset($projects[$project]['recommended'])) {
        $projects[$project]['status'] = UPDATE_UNKNOWN;
        $projects[$project]['reason'] = t('No available releases found');
        continue;
      }

      // Check based upon install type and the site-wide threshold setting.
      $error_level = variable_get('update_notification_threshold', 'all');

      switch ($projects[$project]['install_type']) {
        case 'official':
          if ($projects[$project]['existing_version'] == $projects[$project]['recommended'] || $projects[$project]['existing_version'] == $projects[$project]['latest_version']) {
            $projects[$project]['status'] = UPDATE_CURRENT;
          }
          else {
            if (!empty($projects[$project]['security updates'])) {
              $projects[$project]['status'] = UPDATE_NOT_SECURE;
            }
            else {
              $projects[$project]['status'] = UPDATE_NOT_CURRENT;
            }
          }
          break;
        case 'dev':
          if (!empty($projects[$project]['security updates'])) {
            $projects[$project]['status'] = UPDATE_NOT_SECURE;
            break;
          }

          $latest = $available[$project]['releases'][$projects[$project]['latest_dev']];
          if (empty($projects[$project]['datestamp'])) {
            $projects[$project]['status'] = UPDATE_NOT_CHECKED;
            $projects[$project]['reason'] = t('Unknown release date');
          }
          elseif (($projects[$project]['datestamp'] + 100 > $latest['date'])) {
            $projects[$project]['status'] = UPDATE_CURRENT;
          }
          else {
            $projects[$project]['status'] = UPDATE_NOT_CURRENT;
          }
          break;

        default:
          $projects[$project]['status'] = UPDATE_UNKNOWN;
          $projects[$project]['reason'] = t('Invalid info');
      }
    }
    else {
      $projects[$project]['status'] = UPDATE_UNKNOWN;
      $projects[$project]['reason'] = t('No available releases found');
    }
  }
  // Give other modules a chance to alter the status (for example, to allow a
  // contrib module to provide fine-grained settings to ignore specific
  // projects or releases).
  drupal_alter('update_status', $projects);
  return $projects;
}