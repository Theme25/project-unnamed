#pragma once

// Shared CLI entry point. rbview calls this only in its separate search process;
// simulator globals and Smartforce's signal handler never touch the viewer.
int RbsimMain(int argc, char** argv);
void RequestSmartforceStop();
