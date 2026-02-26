/*
 * =============================================================
 * Busy Light Calendar API - v8.0
 * Fetches M365 ICS feed with caching and weekend detection
 * =============================================================
 */

// ============================================================================
// CONFIGURATION
// ============================================================================

// M365 ICS URL (Outlook > Settings > Calendar > ICS export)
// Run setupICSRrl the first time, after entering unique link.
const ICS_URL = 'YOUR_ICS_URL_HERE';


const TIMEZONE = 'America/New_York';
const BUSINESS_HOURS_START = 8.5;   // 8:30 AM
const BUSINESS_HOURS_END = 16.5;    // 4:30 PM
const CACHE_DURATION_SECONDS = 180; // 3 minutes

// Bump CACHE_VERSION when changing constants above to force cache refresh
// (This is separate from SCRIPT_VERSION)
const CACHE_VERSION = "1";
const SCRIPT_VERSION = "enter version here";

// ============================================================================
// REQUEST HANDLERS
// ============================================================================

function doGet(e) {
  return handleRequest();
}

function doPost(e) {
  return handleRequest();
}

function handleRequest() {
  try {
    var now = new Date();
    var currentHour = now.getHours() + now.getMinutes() / 60;
    var dayOfWeek = now.getDay();
    
    var status = "free";
    var currentMeeting = { title: null, end_time: null };
    var nextMeetingTime = null;
    var nextMeetingTitle = null;
    var isRemoteDay = false;
    var calendarEvents = [];

    // Weekend check (0=Sunday, 6=Saturday)
    if (dayOfWeek === 0 || dayOfWeek === 6) {
      status = "off_hours";
    } else {
      calendarEvents = getCachedCalendarEvents();
      
      // DEBUG - add this temporarily
      Logger.log('Now: ' + now.toISOString());
      for (var i = 0; i < calendarEvents.length; i++) {
      Logger.log('Event: ' + calendarEvents[i].summary + ' | Start: ' + calendarEvents[i].start.toISOString());
      }
      // END DEBUG
      // Check for REMOTE day (event with "REMOTE" before 9 AM today)
      for (var j = 0; j < calendarEvents.length; j++) {
        var evt = calendarEvents[j];
        if (evt.summary.indexOf("REMOTE") !== -1 && 
            isSameDay(evt.start, now) &&
            evt.start.getHours() < 9) {
          isRemoteDay = true;
          break;
        }
      }

      // Determine status
      if (currentHour < BUSINESS_HOURS_START || currentHour >= BUSINESS_HOURS_END) {
        status = "off_hours";
      } else if (isRemoteDay) {
        status = "remote";
      } else {
        // Check for active meetings
        for (var i = 0; i < calendarEvents.length; i++) {
          var evt = calendarEvents[i];
          
          if (evt.isAllDay || 
              evt.summary.indexOf("REMOTE") !== -1 ||
              !isWithinBusinessHours(evt.start)) {
            continue;
          }
          
          // 1 minute grace period before meeting start
          var graceStart = new Date(evt.start.getTime() - 60 * 1000);
          if (now >= graceStart && now < evt.end) {
            // Check if this is an OFF appointment
            if (evt.summary.indexOf("OFF") !== -1) {
              status = "off_hours";
              currentMeeting.title = evt.summary;
              currentMeeting.end_time = formatDateTime(evt.end);
            } else {
              status = "busy";
              currentMeeting.title = evt.summary;
              currentMeeting.end_time = formatDateTime(evt.end);
            }
            break;
          }
        }
      }
      // Find next meeting
      if (!isRemoteDay && status !== "off_hours") {
        for (var i = 0; i < calendarEvents.length; i++) {
          var evt = calendarEvents[i];
          
          if (evt.isAllDay || 
              evt.summary.indexOf("REMOTE") !== -1 ||
              evt.start <= now ||
              !isWithinBusinessHours(evt.start)) {
            continue;
          }
          
          // Skip if before current meeting ends
          if (status === "busy" && currentMeeting.end_time) {
            var currentMeetingEndTime = new Date(currentMeeting.end_time + ' ' + TIMEZONE);
            if (evt.start < currentMeetingEndTime) {
              continue;
            }
          }
          
          nextMeetingTime = evt.start;
          nextMeetingTitle = evt.summary;
          break;
        }
      }
    }

    var response = {
      status: status,
      timestamp: now.toISOString(),
      business_hours: {
        start: BUSINESS_HOURS_START,
        end: BUSINESS_HOURS_END,
        current_hour: currentHour
      },
      remote_day: isRemoteDay,
      current_meeting: currentMeeting,
      next_meeting: {
        time: nextMeetingTime ? formatDateTime(nextMeetingTime) : null,
        title: nextMeetingTitle
      },
      debug_info: {
        total_events: calendarEvents.length,
        cache_age_seconds: getCacheAge(),
        script_version: SCRIPT_VERSION,
        cache_version: CACHE_VERSION
      }
    };
    
    return ContentService.createTextOutput(JSON.stringify(response))
      .setMimeType(ContentService.MimeType.JSON);
      
  } catch (error) {
    Logger.log('ERROR: ' + error.toString());
    return buildErrorResponse(error.toString());
  }
}

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

function isWithinBusinessHours(eventTime) {
  var hour = eventTime.getHours() + (eventTime.getMinutes() / 60);
  return hour >= BUSINESS_HOURS_START && hour < BUSINESS_HOURS_END;
}

function isSameDay(date1, date2) {
  return date1.getFullYear() === date2.getFullYear() &&
         date1.getMonth() === date2.getMonth() &&
         date1.getDate() === date2.getDate();
}

function formatDateTime(date) {
  return Utilities.formatDate(date, TIMEZONE, 'yyyy-MM-dd HH:mm:ss');
}

function buildErrorResponse(message) {
  return ContentService.createTextOutput(JSON.stringify({
    status: "error",
    message: message,
    timestamp: new Date().toISOString()
  })).setMimeType(ContentService.MimeType.JSON);
}

// ============================================================================
// CACHING
// ============================================================================

function getCacheKey() {
  return 'calendar_events_v' + CACHE_VERSION;
}

function getTimestampKey() {
  return 'cache_timestamp_v' + CACHE_VERSION;
}

function getCachedCalendarEvents() {
  var cache = CacheService.getScriptCache();
  var cachedData = cache.get(getCacheKey());
  
  if (cachedData != null) {
    var events = JSON.parse(cachedData);
    events.forEach(function(event) {
      event.start = new Date(event.start);
      event.end = new Date(event.end);
    });
    return events;
  }

  var events = fetchAndParseICS();

  if (events && events.length > 0) {
    cache.put(getCacheKey(), JSON.stringify(events), CACHE_DURATION_SECONDS);
    cache.put(getTimestampKey(), new Date().getTime().toString(), CACHE_DURATION_SECONDS);
  }

  return events;
}

function getCacheAge() {
  var cache = CacheService.getScriptCache();
  var timestamp = cache.get(getTimestampKey());
  if (!timestamp) return null;
  return Math.round((new Date().getTime() - parseInt(timestamp)) / 1000);
}

// ============================================================================
// ICS PARSING
// ============================================================================

function fetchAndParseICS() {
  var scriptProps = PropertiesService.getScriptProperties();
  var icsUrl = scriptProps.getProperty('ICS_URL');
  
  if (!icsUrl || icsUrl === 'YOUR_ICS_URL_HERE') {
    throw new Error("ICS URL not configured. Run setupICSUrl() first.");
  }
  
  var response = UrlFetchApp.fetch(icsUrl, {
    muteHttpExceptions: true,
    followRedirects: true
  });
  
  if (response.getResponseCode() !== 200) {
    throw new Error("ICS fetch failed: " + response.getResponseCode());
  }
  
  return parseICS(response.getContentText());
}

function expandRRULE(event, today, maxDate) {
  var occurrences = [];
  var rrule = event.rrule;
  if (!rrule) return occurrences;
  
  // Parse RRULE components
  var freq = null, interval = 1, byday = null, until = null;
  var parts = rrule.split(';');
  for (var i = 0; i < parts.length; i++) {
    var kv = parts[i].split('=');
    if (kv[0] === 'FREQ') freq = kv[1];
    if (kv[0] === 'INTERVAL') interval = parseInt(kv[1]);
    if (kv[0] === 'BYDAY') byday = kv[1];
    if (kv[0] === 'UNTIL') until = kv[1];
  }
  
  if (!freq) return occurrences;
  
  // Calculate event duration in ms
  var duration = event.end.getTime() - event.start.getTime();
  
  // Store original time components
  var originalHour = event.start.getHours();
  var originalMinute = event.start.getMinutes();
  
  var current = new Date(event.start);
  var loopLimit = 500;
  
  // Helper function to find nth weekday in a month
  function findNthWeekday(year, month, nth, targetDay) {
    var result = new Date(year, month, 1, originalHour, originalMinute, 0);
    
    if (nth > 0) {
      // Positive: find nth occurrence from start of month
      var count = 0;
      while (count < nth) {
        if (result.getDay() === targetDay) count++;
        if (count < nth) result.setDate(result.getDate() + 1);
      }
    } else {
      // Negative: find nth occurrence from end of month
      result = new Date(year, month + 1, 0, originalHour, originalMinute, 0);  // Last day of month
      var count = 0;
      var targetCount = Math.abs(nth);
      while (count < targetCount) {
        if (result.getDay() === targetDay) count++;
        if (count < targetCount) result.setDate(result.getDate() - 1);
      }
    }
    return result;
  }
  
  // For MONTHLY with BYDAY, we need special handling
  if (freq === 'MONTHLY' && byday) {
    var match = byday.match(/(-?\d)?(\w{2})/);
    if (match) {
      var nth = match[1] ? parseInt(match[1]) : 1;
      var dayCode = match[2];
      var days = {SU:0, MO:1, TU:2, WE:3, TH:4, FR:5, SA:6};
      var targetDay = days[dayCode];
      
      // Start from the month of the original event
      var currentYear = event.start.getFullYear();
      var currentMonth = event.start.getMonth();
      
      while (loopLimit-- > 0) {
        current = findNthWeekday(currentYear, currentMonth, nth, targetDay);
        
        // Check UNTIL limit
        if (until) {
          var untilDate = parseICSDateString(until);
          if (untilDate && current > untilDate) break;
        }
        
        // If past maxDate, we're done
        if (current >= maxDate) break;
        
        // If in our window, add it
        if (current >= today && current < maxDate) {
          occurrences.push({
            summary: event.summary,
            start: new Date(current),
            end: new Date(current.getTime() + duration),
            isAllDay: event.isAllDay,
            isException: false,
            busyStatus: event.busyStatus
          });
        }
        
        // Move to next month
        currentMonth += interval;
        if (currentMonth > 11) {
          currentYear += Math.floor(currentMonth / 12);
          currentMonth = currentMonth % 12;
        }
      }
      
      return occurrences;
    }
  }

  // Fast-forward for non-BYDAY monthly or other frequencies
  while (current < today && loopLimit-- > 0) {
    if (freq === 'DAILY') {
      current.setDate(current.getDate() + interval);
    } else if (freq === 'WEEKLY') {
      current.setDate(current.getDate() + (7 * interval));
    } else if (freq === 'MONTHLY') {
      current.setMonth(current.getMonth() + interval);
    } else if (freq === 'YEARLY') {
      current.setFullYear(current.getFullYear() + interval);
    } else {
      break;
    }
  }
  
  // Back up one interval to not miss the first valid occurrence
  if (freq === 'DAILY') {
    current.setDate(current.getDate() - interval);
  } else if (freq === 'WEEKLY') {
    current.setDate(current.getDate() - (7 * interval));
  } else if (freq === 'MONTHLY') {
    current.setMonth(current.getMonth() - interval);
  } else if (freq === 'YEARLY') {
    current.setFullYear(current.getFullYear() - interval);
  }
  
  while (current < maxDate && loopLimit-- > 0) {
    // Check UNTIL limit
    if (until) {
      var untilDate = parseICSDateString(until);
      if (untilDate && current > untilDate) break;
    }
    
    // If this occurrence is in our window, add it
    if (current >= today && current < maxDate) {
      occurrences.push({
        summary: event.summary,
        start: new Date(current),
        end: new Date(current.getTime() + duration),
        isAllDay: event.isAllDay,
        isException: false,
        busyStatus: event.busyStatus
      });
    }
    
    // Advance to next occurrence
    if (freq === 'DAILY') {
      current.setDate(current.getDate() + interval);
    } else if (freq === 'WEEKLY') {
      current.setDate(current.getDate() + (7 * interval));
    } else if (freq === 'MONTHLY') {
      current.setMonth(current.getMonth() + interval);
    } else if (freq === 'YEARLY') {
      current.setFullYear(current.getFullYear() + interval);
    } else {
      break;
    }
  }
  
  return occurrences;
}

function parseICSDateString(dateStr) {
  if (!dateStr || dateStr.length < 8) return null;
  var year = parseInt(dateStr.substring(0, 4));
  var month = parseInt(dateStr.substring(4, 6)) - 1;
  var day = parseInt(dateStr.substring(6, 8));
  
  // Check if there's a time component (T followed by 6 digits)
  if (dateStr.length >= 15 && dateStr.charAt(8) === 'T') {
    var hour = parseInt(dateStr.substring(9, 11));
    var minute = parseInt(dateStr.substring(11, 13));
    var second = parseInt(dateStr.substring(13, 15));
    
    if (dateStr.indexOf('Z') !== -1) {
      return new Date(Date.UTC(year, month, day, hour, minute, second));
    }
    return new Date(year, month, day, hour, minute, second);
  }
  
  return new Date(year, month, day);
}

function parseICS(icsData) {
  var events = [];
  var exceptions = {};  // Track which occurrences are overridden
  var lines = icsData.split(/\r?\n/);
  var currentEvent = null;
  
  // Date range: today to 2 days from now
  var today = new Date();
  today.setHours(0, 0, 0, 0);
  var maxDate = new Date(today);
  maxDate.setDate(maxDate.getDate() + 2);
  
  // First pass: collect all RECURRENCE-ID exceptions
  for (var i = 0; i < lines.length; i++) {
    var line = lines[i].trim();
    
    // Handle line folding
    while (i + 1 < lines.length && (lines[i + 1].charAt(0) === ' ' || lines[i + 1].charAt(0) === '\t')) {
      line += lines[i + 1].substring(1);
      i++;
    }
    
    if (line.indexOf('RECURRENCE-ID') === 0) {
      var exDate = parseICSDate(line);
      if (exDate) {
        // Key by date string to mark this occurrence as overridden
        var key = exDate.toISOString().substring(0, 10);
        exceptions[key] = true;
      }
    }
  }
  
  // Second pass: parse events
  for (var i = 0; i < lines.length; i++) {
    var line = lines[i].trim();
    
    // Handle line folding
    while (i + 1 < lines.length && (lines[i + 1].charAt(0) === ' ' || lines[i + 1].charAt(0) === '\t')) {
      line += lines[i + 1].substring(1);
      i++;
    }
    
    if (line === 'BEGIN:VEVENT') {
      currentEvent = {
        summary: '',
        start: null,
        end: null,
        isAllDay: false,
        isException: false,
        recurrenceId: null,
        busyStatus: 'BUSY',
        rrule: null,
        duration: 0
      };
    } else if (line === 'END:VEVENT') {
      if (currentEvent && currentEvent.start && currentEvent.end) {
        
        // Skip declined meetings (marked as FREE)
        if (currentEvent.busyStatus === 'FREE') {
          currentEvent = null;
          continue;
        }
        
        if (currentEvent.isAllDay) {
          currentEvent.end.setDate(currentEvent.end.getDate() - 1);
        }
        
        // Check if this is within our date range
        if (currentEvent.start >= today && currentEvent.start < maxDate) {
          // If this event has a RECURRENCE-ID, it's an exception (replacement)
          // Always include these - they represent moved/modified occurrences
          if (currentEvent.isException) {
            events.push(currentEvent);
          } else {
            // For regular/recurring events, check if this occurrence was overridden
            var dateKey = currentEvent.start.toISOString().substring(0, 10);
            if (!exceptions[dateKey]) {
              events.push(currentEvent);
            }
          }
        } else if (currentEvent.rrule && !currentEvent.isException) {
          // Event is outside our window but has RRULE - expand it
          var expanded = expandRRULE(currentEvent, today, maxDate);
          for (var e = 0; e < expanded.length; e++) {
            var occ = expanded[e];
            var occKey = occ.start.toISOString().substring(0, 10);
            if (!exceptions[occKey]) {
              events.push(occ);
            }
          }
        }
      }
      currentEvent = null;
    } else if (currentEvent) {
      if (line.indexOf('SUMMARY:') === 0) {
        currentEvent.summary = line.substring(8).trim();
      } else if (line.indexOf('DTSTART') === 0) {
        currentEvent.start = parseICSDate(line);
        if (line.indexOf('VALUE=DATE') !== -1) {
          currentEvent.isAllDay = true;
        }
      } else if (line.indexOf('DTEND') === 0) {
        currentEvent.end = parseICSDate(line);
      } else if (line.indexOf('RECURRENCE-ID') === 0) {
        currentEvent.isException = true;
        currentEvent.recurrenceId = parseICSDate(line);
      } else if (line.indexOf('X-MICROSOFT-CDO-BUSYSTATUS:') === 0) {
        currentEvent.busyStatus = line.substring(27).trim();
      } else if (line.indexOf('RRULE:') === 0) {
        currentEvent.rrule = line.substring(6).trim();
      }
    }
  }
  
  // Sort by start time
  events.sort(function(a, b) {
    return a.start - b.start;
  });
  
  Logger.log('Parsed ' + events.length + ' events from ICS');
  return events;
}

function parseICSDate(line) {
  var match = line.match(/[:;](\d{8}T\d{6}Z?)/) || line.match(/[:;](\d{8})/);
  if (!match) return null;
  
  var dateStr = match[1];
  var year = parseInt(dateStr.substring(0, 4));
  var month = parseInt(dateStr.substring(4, 6)) - 1;
  var day = parseInt(dateStr.substring(6, 8));
  
  if (dateStr.length === 8) {
    return new Date(year, month, day);
  }
  
  var hour = parseInt(dateStr.substring(9, 11));
  var minute = parseInt(dateStr.substring(11, 13));
  var second = parseInt(dateStr.substring(13, 15));
  
  if (dateStr.indexOf('Z') !== -1) {
    return new Date(Date.UTC(year, month, day, hour, minute, second));
  }
  return new Date(year, month, day, hour, minute, second);
}

// ============================================================================
// SETUP & TESTING (Run manually)
// ============================================================================

function setupICSUrl() {
  PropertiesService.getScriptProperties().setProperty('ICS_URL', ICS_URL);
  Logger.log('ICS URL stored. Deploy as Web App with "Anyone" permissions.');
}

function clearCache() {
  var cache = CacheService.getScriptCache();
  cache.remove(getCacheKey());
  cache.remove(getTimestampKey());
  Logger.log('Cache cleared (version ' + CACHE_VERSION + ')');
}