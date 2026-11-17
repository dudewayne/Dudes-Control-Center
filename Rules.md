Before we start with the project, let me define some basic rules

1. Always follow the rules! No exceptions! This is the most important rule of all!
2. Create log files for each application, having detailled logfiles is mandatory for debugging.
3. You have access to the log files, so use this. That means, if you test something by yourself, check the logs by yourself. If you ask me to test something, don't ask me to check the logs for you, do it yourself, after i confirmed that i executed the test.
4. Always create a deb file after you changed something, and install it before you ask me to test something. You can store the deb files within a subfolder.
5. When creating a new version, remove the old logs for the next test, otherwise this might be confusing during testing.
6. If there is a daemon application in the project (i.e. running through systemd), always make sure that it starts automatically after installation and also restarts automatically after reinstallation.
7. If there are kernel modules in the application, make sure they get build properly during installation and rebuild properly during reinstallation.
8. If you have to compile something, use all cpu cores available.
9. Always create a backup in a separate backup subfolder before you change anything, so you can go back without issues. All backups will be created in this backup subfolder within their respective subfolders. Keep a maximum of 5 backups. Include all project files in these backup folders. Naming convention: backup_YYYY-MM-DD_versionnumber_fix (e.g., backup_2026-11-17_v1.0_rename_to_control_center)
10. Implement a versioning system and increase the version by 1 minor version if we add a new feature that works. Keep a detailled changelog to document the progress.
11. If we succesfully implemented a new feature, create a full backup of the project in the folder backup_archive. Use subfolders for each full backup you make. Naming convention: fullbackup_YYYY-MM-DD_versionnumber_LastFeature (e.g., fullbackup_2026-11-17_v1.1.0_rename_to_control_center)
12. Make small steps, don't try to change too much at the same time.
13. Use git for version control. Commit changes after each meaningful step with clear commit messages. Push changes to the remote repository after successful commits.
14. Create a GitHub release for every new version. Tag the release with the version number (e.g., v1.5.1), include release notes, and attach the built .deb file.
